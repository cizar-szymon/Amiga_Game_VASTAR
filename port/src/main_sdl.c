/*
 * Vastar port - SDL2 frontend for the PC (development and testing).
 *
 *   vastar [options] [SET | FILE.zip]     SET = vastar (default), vastar2, vastar3, vastar4, pprobe
 *
 *   -rompath DIR        where to look for SET.zip, SET/ (also of the parent set) or the loose
 *                       ROM files; may be repeated, default: . roms rom
 *   -scale N            window scale (default 3)
 *   -dsw1 HH -dsw2 HH   DIP switches (hex, default from MAME)
 *   -freeplay           no coins needed (DIP switch); fire also starts a game
 *   -p2separate         player 2 needs its own controls (as in the arcade); by default
 *                       player 2 plays with player 1's (they take turns anyway)
 *   -lives N            lives at the start of a game, 1-9 (not in the original: the DIP
 *                       switches allow 3 to 6); -lives inf: lives do not run out
 *   -nosound
 *   -frames N           quit after N frames
 *   -fast               no 60 Hz pacing
 *   -headless           no window and no sound (for automated runs)
 *   -shots DIR F1,F2..  save the screen as DIR/frame_NNNNNN.bmp at these frames
 *   -wav FILE           also write the sound to a WAV file
 *   -script LIST        timed inputs, e.g. "300:+coin1,310:-coin1,400:+start1,405:-start1"
 *                       (names: left right up down b1 b2 coin1 coin2 service start1 start2)
 *   -stats              print the emulation time per frame every 2 seconds
 *   -check-render       compare the fast renderer with the reference one every frame
 *
 * Keys: cursor keys = joystick, Left Ctrl / Z / Space = fire, Left Alt / X = button 2,
 *       5 / 6 = coin, 1 / 2 = start, 9 = service, P = pause, F3 = reset, Esc = quit.
 * A game controller works too (A = fire, B = button 2, Back = coin, Start = start).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "vastar.h"
#include "video.h"
#include "romload.h"

#define SAMPLE_RATE 44100

static Vastar machine;
static Video video;
static uint8_t fb[VIDEO_W * VIDEO_H];
static uint8_t screen[SCREEN_W * SCREEN_H];
static uint32_t rgb[SCREEN_W * SCREEN_H];
static uint8_t screen_ref[SCREEN_W * SCREEN_H];
static int check_render;
static unsigned long stat_blocks;
extern unsigned long video_stat_tiles;
static unsigned long check_frames, check_bad;
static int16_t audio[SAMPLE_RATE / 50];

/* ---- options ---- */

static const char *set_name = "vastar";
static const char *zip_arg;
static const char *rompaths[16];
static int nrompaths;
static int scale = 3, nosound, fast, headless, stats;
static long max_frames = -1;
static int dsw1 = -1, dsw2 = -1, lives, infinite_lives, freeplay, p2separate;
static const char *shot_dir;
static long shot_frames[64];
static int shot_count;
static const char *wav_path;
static FILE *wav_fp;
static unsigned long wav_bytes;

typedef struct { long frame; uint8_t *port; uint8_t bit; int press; } ScriptEvent;
static ScriptEvent script[128];
static int script_count;

static uint8_t kb_p1, kb_sys, pad_p1, pad_sys, script_p1, script_sys;

static void usage(void)
{
    fprintf(stderr, "usage: vastar [-rompath DIR] [-scale N] [-dsw1 HH] [-dsw2 HH] [-nosound] [-frames N]\n"
                    "              [-fast] [-headless] [-shots DIR F1,F2..] [-wav FILE] [-script LIST]\n"
                    "              [-stats] [SET | FILE.zip]\nsets:");
    {
        int i;
        for (i = 0; i < vastar_game_count; i++)
            fprintf(stderr, " %s", vastar_games[i].name);
    }
    fprintf(stderr, "\n");
    exit(2);
}

static int input_bit(const char *name, uint8_t **port, uint8_t *bit)
{
    static const struct { const char *name; int sys; uint8_t bit; } names[] = {
        { "left", 0, IN_LEFT }, { "right", 0, IN_RIGHT }, { "up", 0, IN_UP }, { "down", 0, IN_DOWN },
        { "b1", 0, IN_BUTTON1 }, { "b2", 0, IN_BUTTON2 }, { "coin1", 1, IN_COIN1 },
        { "coin2", 1, IN_COIN2 }, { "service", 1, IN_SERVICE }, { "start1", 1, IN_START1 },
        { "start2", 1, IN_START2 }
    };
    int i;
    for (i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
        if (!strcmp(name, names[i].name)) {
            *port = names[i].sys ? &script_sys : &script_p1;
            *bit = names[i].bit;
            return 1;
        }
    return 0;
}

static void parse_script(const char *s)
{
    char buf[1024], *tok;
    snprintf(buf, sizeof buf, "%s", s);
    for (tok = strtok(buf, ","); tok && script_count < 128; tok = strtok(0, ",")) {
        ScriptEvent *e = &script[script_count];
        char *colon = strchr(tok, ':');
        if (!colon || (colon[1] != '+' && colon[1] != '-') || !input_bit(colon + 2, &e->port, &e->bit)) {
            fprintf(stderr, "bad script entry: %s\n", tok);
            exit(2);
        }
        e->frame = atol(tok);
        e->press = colon[1] == '+';
        script_count++;
    }
}

static void parse_args(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (usage(), ""))
        if (!strcmp(a, "-rompath")) { if (nrompaths < 8) rompaths[nrompaths++] = NEXT(); }
        else if (!strcmp(a, "-scale")) scale = atoi(NEXT());
        else if (!strcmp(a, "-dsw1")) dsw1 = (int)strtol(NEXT(), 0, 16);
        else if (!strcmp(a, "-dsw2")) dsw2 = (int)strtol(NEXT(), 0, 16);
        else if (!strcmp(a, "-nosound")) nosound = 1;
        else if (!strcmp(a, "-freeplay")) freeplay = 1;
        else if (!strcmp(a, "-p2separate")) p2separate = 1;
        else if (!strcmp(a, "-lives")) {
            const char *l = NEXT();
            if (!strcmp(l, "inf")) infinite_lives = 1;
            else lives = atoi(l);
        }
        else if (!strcmp(a, "-frames")) max_frames = atol(NEXT());
        else if (!strcmp(a, "-fast")) fast = 1;
        else if (!strcmp(a, "-headless")) headless = fast = nosound = 1;
        else if (!strcmp(a, "-stats")) stats = 1;
        else if (!strcmp(a, "-check-render")) check_render = 1;
        else if (!strcmp(a, "-wav")) wav_path = NEXT();
        else if (!strcmp(a, "-script")) parse_script(NEXT());
        else if (!strcmp(a, "-shots")) {
            char buf[1024], *tok;
            shot_dir = NEXT();
            snprintf(buf, sizeof buf, "%s", NEXT());
            for (tok = strtok(buf, ","); tok && shot_count < 64; tok = strtok(0, ","))
                shot_frames[shot_count++] = atol(tok);
        }
        else if (a[0] == '-') usage();
        else {
            size_t n = strlen(a);
            if (n > 4 && !strcmp(a + n - 4, ".zip"))
                zip_arg = a;
            else
                set_name = a;
        }
#undef NEXT
    }
    if (scale < 1)
        scale = 1;
}

/* ---- ROMs ---- */

static int load_roms(void)
{
    const GameDef *game;
    static char paths[64][512];
    static char exe_paths[5][512];
    const char *src[64];
    int n = 0, i, d;

    if (zip_arg) {
        /* set name = zip file name without .zip */
        static char name[128];
        const char *base = zip_arg, *p;
        for (p = zip_arg; *p; p++)
            if (*p == '/' || *p == '\\' || *p == ':')
                base = p + 1;
        snprintf(name, sizeof name, "%.*s", (int)(strlen(base) - 4), base);
        set_name = name;
        snprintf(paths[n], 512, "%s", zip_arg);
        src[n] = paths[n];
        n++;
    }
    game = vastar_find_game(set_name);
    if (!game) {
        fprintf(stderr, "unknown set: %s\n", set_name);
        usage();
    }
    if (nrompaths == 0) {
        rompaths[nrompaths++] = ".";
        rompaths[nrompaths++] = "roms";
        rompaths[nrompaths++] = "rom";
        /* also next to the program and in the project directory (build/port/../..),
           so that a double click works */
        {
            char *base = SDL_GetBasePath();
            if (base) {
                static const char *const rel[5] = { "", "rom", "roms", "../../rom", "../../roms" };
                for (i = 0; i < 5; i++) {
                    path_join(exe_paths[i], 512, base, rel[i]);
                    rompaths[nrompaths++] = exe_paths[i];
                }
                SDL_free(base);
            }
        }
    }
    for (d = 0; d < nrompaths; d++) {
        const char *names[2];
        names[0] = game->name;
        names[1] = game->parent;
        for (i = 0; i < 2; i++) {
            char file[160];
            if (!names[i] || n + 2 > 64)
                continue;
            snprintf(file, sizeof file, "%s.zip", names[i]);
            path_join(paths[n], 512, rompaths[d], file);
            src[n] = paths[n];
            n++;
            path_join(paths[n], 512, rompaths[d], names[i]);
            src[n] = paths[n];
            n++;
        }
        /* loose ROM files directly in the directory */
        if (n < 64) {
            snprintf(paths[n], 512, "%s", rompaths[d]);
            src[n] = paths[n];
            n++;
        }
    }
    if (vastar_load_roms(&machine, game, src, n)) {
        char msg[2048];
        int len = snprintf(msg, sizeof msg, "Cannot load the ROMs of %s.\n\nPut %s.zip or the ROM files "
                           "into a rom folder next to the program or give -rompath DIR.\n\nLooked in:\n",
                           game->title, game->name);
        for (d = 0; d < nrompaths && len < (int)sizeof msg - 1; d++)
            len += snprintf(msg + len, sizeof msg - (size_t)len, "%s\n", rompaths[d]);
        fprintf(stderr, "%s", msg);
        if (!headless)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Vastar", msg, 0);
        return 0;
    }
    printf("%s: ROMs loaded\n", game->title);
    return 1;
}

/* ---- files ---- */

static void put16(FILE *fp, unsigned v) { fputc((int)(v & 0xff), fp); fputc((int)((v >> 8) & 0xff), fp); }
static void put32(FILE *fp, unsigned long v) { put16(fp, (unsigned)(v & 0xffff)); put16(fp, (unsigned)(v >> 16)); }

static void save_bmp(const char *path)
{
    FILE *fp = fopen(path, "wb");
    int x, y;
    unsigned long size = (unsigned long)SCREEN_W * 3 * SCREEN_H;   /* 224 * 3 is a multiple of 4 */
    if (!fp) {
        perror(path);
        return;
    }
    fputc('B', fp); fputc('M', fp);
    put32(fp, 54 + size); put32(fp, 0); put32(fp, 54);
    put32(fp, 40); put32(fp, SCREEN_W); put32(fp, SCREEN_H); put16(fp, 1); put16(fp, 24);
    put32(fp, 0); put32(fp, size); put32(fp, 2835); put32(fp, 2835); put32(fp, 0); put32(fp, 0);
    for (y = SCREEN_H - 1; y >= 0; y--)
        for (x = 0; x < SCREEN_W; x++) {
            const uint8_t *c = video.palette[screen[y * SCREEN_W + x]];
            fputc(c[2], fp); fputc(c[1], fp); fputc(c[0], fp);
        }
    fclose(fp);
}

static void wav_header(FILE *fp, unsigned long bytes)
{
    fwrite("RIFF", 1, 4, fp); put32(fp, 36 + bytes); fwrite("WAVEfmt ", 1, 8, fp);
    put32(fp, 16); put16(fp, 1); put16(fp, 1); put32(fp, SAMPLE_RATE); put32(fp, SAMPLE_RATE * 2);
    put16(fp, 2); put16(fp, 16); fwrite("data", 1, 4, fp); put32(fp, bytes);
}

/* ---- input ---- */

static void set_bit(uint8_t *v, uint8_t bit, int on)
{
    if (on)
        *v |= bit;
    else
        *v &= (uint8_t)~bit;
}

static int handle_key(SDL_Keycode k, int down)
{
    switch (k) {
    case SDLK_LEFT: set_bit(&kb_p1, IN_LEFT, down); break;
    case SDLK_RIGHT: set_bit(&kb_p1, IN_RIGHT, down); break;
    case SDLK_UP: set_bit(&kb_p1, IN_UP, down); break;
    case SDLK_DOWN: set_bit(&kb_p1, IN_DOWN, down); break;
    case SDLK_LCTRL: case SDLK_z: case SDLK_SPACE: set_bit(&kb_p1, IN_BUTTON1, down); break;
    case SDLK_LALT: case SDLK_x: set_bit(&kb_p1, IN_BUTTON2, down); break;
    case SDLK_5: set_bit(&kb_sys, IN_COIN1, down); break;
    case SDLK_6: set_bit(&kb_sys, IN_COIN2, down); break;
    case SDLK_9: set_bit(&kb_sys, IN_SERVICE, down); break;
    case SDLK_1: set_bit(&kb_sys, IN_START1, down); break;
    case SDLK_2: set_bit(&kb_sys, IN_START2, down); break;
    default: return 0;
    }
    return 1;
}

static void read_pad(SDL_GameController *pad)
{
    int ax, ay;
    pad_p1 = pad_sys = 0;
    if (!pad)
        return;
    ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || ax < -16000) pad_p1 |= IN_LEFT;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax > 16000) pad_p1 |= IN_RIGHT;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) || ay < -16000) pad_p1 |= IN_UP;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ay > 16000) pad_p1 |= IN_DOWN;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) pad_p1 |= IN_BUTTON1;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B)) pad_p1 |= IN_BUTTON2;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK)) pad_sys |= IN_COIN1;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) pad_sys |= IN_START1;
}

/* ---- main ---- */

int main(int argc, char **argv)
{
    SDL_Window *window = 0;
    SDL_Renderer *renderer = 0;
    SDL_Texture *texture = 0;
    SDL_AudioDeviceID audio_dev = 0;
    SDL_GameController *pad = 0;
    Uint64 freq, next, t0, t_emu = 0;
    long frame = 0;
    int running = 1, paused = 0, next_shot = 0, next_event = 0, i;

    parse_args(argc, argv);
    if (!load_roms())
        return 1;
    vastar_init(&machine, SAMPLE_RATE);
    if (dsw1 >= 0) machine.dsw1 = (uint8_t)dsw1;
    if (dsw2 >= 0) machine.dsw2 = (uint8_t)dsw2;
    if (freeplay)
        machine.dsw2 &= 0xf8;                       /* coin A: free play */
    machine.lives = (uint8_t)(lives < 0 ? 0 : lives > 9 ? 9 : lives);
    machine.infinite_lives = (uint8_t)infinite_lives;
    machine.p2_from_p1 = (uint8_t)!p2separate;
    video_init(&video, &machine);

    if (SDL_Init(headless ? SDL_INIT_TIMER : SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!headless) {
        char title[96];
        snprintf(title, sizeof title, "%s", machine.game->title);
        window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  SCREEN_W * scale, SCREEN_H * scale, SDL_WINDOW_RESIZABLE);
        renderer = SDL_CreateRenderer(window, -1, 0);
        SDL_RenderSetLogicalSize(renderer, SCREEN_W, SCREEN_H);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                    SCREEN_W, SCREEN_H);
        for (i = 0; i < SDL_NumJoysticks(); i++)
            if (SDL_IsGameController(i) && (pad = SDL_GameControllerOpen(i)) != 0)
                break;
    }
    if (!nosound) {
        SDL_AudioSpec want, have;
        memset(&want, 0, sizeof want);
        want.freq = SAMPLE_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = 1024;
        audio_dev = SDL_OpenAudioDevice(0, 0, &want, &have, 0);
        if (audio_dev)
            SDL_PauseAudioDevice(audio_dev, 0);
        else
            fprintf(stderr, "no sound: %s\n", SDL_GetError());
    }
    if (wav_path) {
        wav_fp = fopen(wav_path, "wb");
        if (wav_fp)
            wav_header(wav_fp, 0);
    }

    freq = SDL_GetPerformanceFrequency();
    next = SDL_GetPerformanceCounter();
    while (running) {
        SDL_Event ev;
        int n;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                running = 0;
            else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
                SDL_Keycode k = ev.key.keysym.sym;
                int down = ev.type == SDL_KEYDOWN;
                if (handle_key(k, down))
                    continue;
                if (!down || ev.key.repeat)
                    continue;
                if (k == SDLK_ESCAPE)
                    running = 0;
                else if (k == SDLK_p)
                    paused = !paused;
                else if (k == SDLK_F3)
                    vastar_reset(&machine);
            } else if (ev.type == SDL_CONTROLLERDEVICEADDED && !pad) {
                pad = SDL_GameControllerOpen(ev.cdevice.which);
            }
        }
        read_pad(pad);

        while (next_event < script_count && script[next_event].frame <= frame) {
            set_bit(script[next_event].port, script[next_event].bit, script[next_event].press);
            next_event++;
        }
        machine.in_p1 = (uint8_t)(kb_p1 | pad_p1 | script_p1);
        machine.in_p2 = 0;
        machine.in_system = (uint8_t)(kb_sys | pad_sys | script_sys);
        if (freeplay && (pad_p1 & IN_BUTTON1))
            machine.in_system |= IN_START1;          /* the pad's fire starts a game */

        if (!paused) {
            t0 = SDL_GetPerformanceCounter();
            n = vastar_run_frame(&machine, audio);
            stat_blocks += (unsigned long)video_render_screen(&video, &machine, screen);
            if (check_render) {
                video_render(&video, &machine, fb);
                video_rotate(fb, screen_ref, SCREEN_W);
                check_frames++;
                if (memcmp(screen, screen_ref, sizeof screen)) {
                    if (check_bad++ < 5)
                        printf("check-render: frame %ld differs\n", frame);
                }
            }
            t_emu += SDL_GetPerformanceCounter() - t0;
            if (audio_dev) {
                /* keep the queue short: drop audio if we got ahead */
                if (SDL_GetQueuedAudioSize(audio_dev) < SAMPLE_RATE / 5 * 2)
                    SDL_QueueAudio(audio_dev, audio, (Uint32)n * 2);
            }
            if (wav_fp) {
                fwrite(audio, 2, (size_t)n, wav_fp);   /* little endian PC */
                wav_bytes += (unsigned long)n * 2;
            }
            while (next_shot < shot_count && shot_frames[next_shot] <= frame) {
                char path[512], file[64];
                if (shot_frames[next_shot] == frame) {
                    snprintf(file, sizeof file, "frame_%06ld.bmp", frame);
                    path_join(path, sizeof path, shot_dir, file);
                    save_bmp(path);
                }
                next_shot++;
            }
            frame++;
            if (stats && frame % 121 == 0) {
                printf("frame %ld: %.2f ms per frame, %lu blocks, %lu tiles redrawn per frame\n", frame,
                       (double)t_emu * 1000.0 / freq / 121, stat_blocks / 121, video_stat_tiles / 121);
                t_emu = 0;
                stat_blocks = video_stat_tiles = 0;
            }
            if (max_frames >= 0 && frame >= max_frames)
                running = 0;
        }

        if (!headless) {
            void *pixels;
            int pitch;
            if (SDL_LockTexture(texture, 0, &pixels, &pitch) == 0) {
                for (i = 0; i < SCREEN_W * SCREEN_H; i++) {
                    const uint8_t *c = video.palette[screen[i]];
                    rgb[i] = 0xff000000u | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2];
                }
                for (i = 0; i < SCREEN_H; i++)
                    memcpy((uint8_t *)pixels + i * pitch, rgb + i * SCREEN_W, SCREEN_W * 4);
                SDL_UnlockTexture(texture);
            }
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, 0, 0);
            SDL_RenderPresent(renderer);
        }

        if (!fast) {
            Uint64 now;
            next += freq * 1000 / VASTAR_FPS_X1000;
            now = SDL_GetPerformanceCounter();
            if (next > now)
                SDL_Delay((Uint32)((next - now) * 1000 / freq));
            else if (now - next > freq / 4)
                next = now;   /* far behind: do not try to catch up */
        }
    }

    if (check_render)
        printf("check-render: %lu frames, %lu with differences\n", check_frames, check_bad);
    if (wav_fp) {
        fseek(wav_fp, 0, SEEK_SET);
        wav_header(wav_fp, wav_bytes);
        fclose(wav_fp);
    }
    if (pad)
        SDL_GameControllerClose(pad);
    if (audio_dev)
        SDL_CloseAudioDevice(audio_dev);
    SDL_Quit();
    return 0;
}
