/*
 * Vastar port - AmigaOS frontend: a window on the Workbench.
 *
 * Built on the Wizard of Wor Amiga frontend: Intuition window, Picasso96 (BLIT or
 * WPA) or WriteChunkyPixels for the picture, timer.device for the 60.606 Hz pace,
 * lowlevel.library for the joystick, audio.device (Paula, default) or the AHI library.
 * Needs AmigaOS 3.1+ (V40) and a 68020+ (built for the 68060); AHI is optional.
 *
 * Shell:  Vastar [SET] [SCALE=1|2|3] [ROMPATH=dir] [NOSOUND] [SOUND=PAULA|AHI]
 *                [VIDEO=BLIT|WPA|WCP] [FRAMESKIP=AUTO|n] [SCREEN=WINDOW|FULL] [LIVES=1..9|INFINITE]
 *                [FREEPLAY] [JOYPORT=BOTH] [DSW1=hh] [DSW2=hh] [STATS|STATS=FULL]
 *         SCREEN=FULL: an own screen (mode of the graphics card), picture centred
 *         LIVES: lives at the start of a game (the DIP switches allow 3 to 6), INFINITE
 *         FREEPLAY: no coins needed, the joystick's fire button starts a game
 *         JOYPORT=BOTH: a joystick in the mouse port works too
 *         player 2 (the game alternates) plays with the same joystick and keys;
 *         P2=SEPARATE: only with the arcade's own P2 controls (none here)
 *         FRAMESKIP: the game always runs at 60.6 Hz; AUTO (default) leaves out pictures
 *         when the Amiga falls behind, n draws one picture of n+1
 *         SET = vastar (default), vastar2, vastar3, vastar4, pprobe
 *         the ROMs (SET.zip from MAME, a SET drawer or the loose files) are looked for in
 *         ROMPATH, PROGDIR:, PROGDIR:rom and PROGDIR:roms
 * Icon:   the same as tool types ("(...)" is switched off)
 *
 * Joystick in port 2: move, fire = fire, second button = button 2
 *   (CD32 pad: red fire, blue button 2, play = start 1)
 * Keyboard: cursor keys, Ctrl / Space / Z = fire, Alt / X = button 2, 5 / 6 = coin,
 *           1 / 2 = start, 9 = service, P = pause, F3 = reset, Esc / close / Ctrl-C = quit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <devices/ahi.h>
#include <devices/audio.h>
#include <clib/alib_protos.h>
#include <devices/inputevent.h>
#include <intuition/intuition.h>
#include <graphics/gfx.h>
#include <graphics/modeid.h>
#include <graphics/displayinfo.h>
#include <libraries/lowlevel.h>
#include <proto/exec.h>
#include <proto/icon.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/lowlevel.h>
#include <proto/timer.h>
#include <proto/ahi.h>
#include <libraries/Picasso96.h>
#include <proto/Picasso96.h>
#include <utility/hooks.h>

#include "vastar.h"
#include "video.h"
#include "romload.h"

#define AUDIO_RATE 22050

const char version[] = "$VER: Vastar 0.1 (09.10.2026) arcade emulation";

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *LowLevelBase;
struct Device *TimerBase;
struct Library *AHIBase;
struct Library *P96Base;

/* the picture: 224x256, blocks of 8x8 */
#define RENDER_W SCREEN_W
#define RENDER_H SCREEN_H
#define RENDER_BW (RENDER_W / 8)
#define RENDER_BH (RENDER_H / 8)

static Vastar machine;
static Video video;
/* long aligned */
static ULONG indexed_l[RENDER_W * RENDER_H / 4];   /* rotated, what the window should show */
#define indexed ((uint8_t *)indexed_l)
static int16_t audio[AUDIO_RATE / 50];
static int force_full = 1;

static struct Window *win;
static LONG pens[256];
static int pens_ok;
static int scale = 2;
static UBYTE *chunky;

static struct MsgPort *timer_port;
static struct timerequest *timer_req;
static ULONG eclock_freq;
static uint64_t next_frame;

static UBYTE keys[128];
static ULONG joy_state;

static int stats, paused, sound = 1;
static int frameskip = -1;            /* FRAMESKIP=n: draw 1 of n+1 frames; -1 = automatic */
#define MAX_AUTO_SKIP 3
static ULONG stat_drawn;
static const char *set_name = "vastar";
static const char *rompaths[4];
static int nrompaths;
static int dsw1 = -1, dsw2 = -1;
static int lives, infinite_lives;     /* LIVES=1..9 | INFINITE (not in the original) */
static int freeplay;                  /* FREEPLAY: no coins; fire starts a game */
static int p2separate;                /* P2=SEPARATE: player 2 needs the arcade's P2 controls */
static int joy_mouseport;             /* JOYPORT=BOTH: also a joystick in the mouse port */
static int screen_full;               /* SCREEN=FULL: an own screen instead of a window */
static struct Screen *scr;
static UWORD *blank_pointer;
static int ox, oy;                    /* where the picture starts in the window */

static void close_sound(void);
static void close_rtg(void);
static void prof_stage(int stage);
static uint64_t eclock_now(void);

/* ------------------------------------------------------------------ watchdog */

/*
 * Diagnostics for hangs on real hardware (as in Wizard of Wor): after 3 s without
 * a new frame the state of the game task and its stack go to RAM:Vastar-hang.txt.
 */
static volatile ULONG diag_frames;
static struct Task *main_task;
static volatile int wd_quit;
static BYTE wd_done_sig = -1;
static struct Process *wd_proc;

int main(int argc, char **argv);

static void report_hang(void)
{
    static ULONG stack[160];
    static char text[4096];
    struct Task *t = main_task;
    struct Process *pr = (struct Process *)t;
    ULONG state, sigwait, sigrecvd, sp, upper, n = 0, i;
    int len = 0;
    BPTR seg, fh;

    Disable();
    state = t->tc_State;
    sigwait = t->tc_SigWait;
    sigrecvd = t->tc_SigRecvd;
    sp = (ULONG)t->tc_SPReg;
    upper = (ULONG)t->tc_SPUpper;
    while (n < 160 && sp + n * 4 + 4 <= upper) {
        stack[n] = ((ULONG *)sp)[n];
        n++;
    }
    Enable();

    len += sprintf(text + len, "Vastar hang: frames %lu state %lu sigwait %08lx sigrecvd %08lx sp %08lx upper %08lx\n",
                   diag_frames, state, sigwait, sigrecvd, sp, upper);
    len += sprintf(text + len, "main %08lx main pc %04x sub pc %04x\n", (ULONG)main,
                   (unsigned)machine.main.pc, (unsigned)machine.sub.pc);
    seg = pr->pr_CLI ? ((struct CommandLineInterface *)BADDR(pr->pr_CLI))->cli_Module
                     : ((BPTR *)BADDR(pr->pr_SegList))[3];
    for (i = 0; seg && i < 8; i++) {
        ULONG *sl = (ULONG *)BADDR(seg);
        len += sprintf(text + len, "segment %lu: %08lx size %lu\n", i, (ULONG)(sl + 1), sl[-1]);
        seg = (BPTR)sl[0];
    }
    for (i = 0; i < n; i++)
        len += sprintf(text + len, "%08lx%s", stack[i], i % 8 == 7 ? "\n" : " ");
    len += sprintf(text + len, "\n");

    fh = Open((STRPTR)"RAM:Vastar-hang.txt", MODE_NEWFILE);
    if (fh) {
        Write(fh, text, len);
        Close(fh);
    }
}

static void watchdog(void)
{
    ULONG last = diag_frames;
    int still = 0, reported = 0;

    while (!wd_quit) {
        Delay(25);
        if (diag_frames != last || paused) {
            last = diag_frames;
            still = 0;
        } else if (++still == 6 && !reported) {
            reported = 1;
            report_hang();
        }
    }
    Forbid();                           /* the game may unload us only after we are gone */
    Signal(main_task, 1UL << wd_done_sig);
}

static void start_watchdog(void)
{
    main_task = FindTask(NULL);
    wd_done_sig = AllocSignal(-1);
    if (wd_done_sig < 0)
        return;
    wd_proc = CreateNewProcTags(NP_Entry, (ULONG)watchdog, NP_Name, (ULONG)"Vastar watchdog",
                                NP_Priority, 1, NP_StackSize, 8192, TAG_DONE);
}

static void stop_watchdog(void)
{
    if (wd_proc) {
        wd_quit = 1;
        Wait(1UL << wd_done_sig);
        wd_proc = NULL;
    }
    if (wd_done_sig >= 0)
        FreeSignal(wd_done_sig);
    wd_done_sig = -1;
}

/* ------------------------------------------------------------------ cleanup */

static void cleanup(void)
{
    int i;

    stop_watchdog();
    close_sound();
    if (timer_req) {
        CloseDevice((struct IORequest *)timer_req);
        DeleteIORequest((struct IORequest *)timer_req);
    }
    if (timer_port)
        DeleteMsgPort(timer_port);
    close_rtg();
    if (win) {
        if (pens_ok)
            for (i = 0; i < 256; i++)
                if (pens[i] >= 0)
                    ReleasePen(win->WScreen->ViewPort.ColorMap, (ULONG)pens[i]);
        if (blank_pointer)
            ClearPointer(win);
        CloseWindow(win);
    }
    if (blank_pointer)
        FreeVec(blank_pointer);
    if (scr)
        CloseScreen(scr);
    if (chunky)
        FreeVec(chunky);
    if (LowLevelBase)
        CloseLibrary(LowLevelBase);
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
}

static void fail(const char *msg)
{
    struct EasyStruct es;
    if (IntuitionBase) {
        es.es_StructSize = sizeof es;
        es.es_Flags = 0;
        es.es_Title = (UBYTE *)"Vastar";
        es.es_TextFormat = (UBYTE *)"%s";
        es.es_GadgetFormat = (UBYTE *)"OK";
        EasyRequest(NULL, &es, NULL, (ULONG)msg);
    } else {
        printf("%s\n", msg);
    }
    exit(20);
}

/* ------------------------------------------------------------------ video */

/* SCREEN=FULL: an own screen in the best mode for the picture (on RTG systems a mode
   of the graphics card, 16 bit preferred), a borderless backdrop window on it, the
   picture centred, no mouse pointer */
static void open_fullscreen(void)
{
    static ULONG black[] = { 1UL << 16, 0, 0, 0, 0 };
    const ULONG w = (ULONG)(RENDER_W * scale), h = (ULONG)(RENDER_H * scale);
    ULONG id = INVALID_ID, mode = INVALID_ID, depth = 8, best_area = 0xffffffffUL;
    int best_rank = 0;
    struct DimensionInfo dims;

    /* the smallest available mode that holds the picture; 16 bit before 32/24 bit
       before 8 bit (8 bit needs pens for the 84 colours of the game) */
    while ((id = NextDisplayInfo(id)) != INVALID_ID) {
        struct DisplayInfo disp;
        ULONG mw, mh, area;
        int rank;
        if (!GetDisplayInfoData(NULL, (UBYTE *)&disp, sizeof disp, DTAG_DISP, id) || disp.NotAvailable ||
            !GetDisplayInfoData(NULL, (UBYTE *)&dims, sizeof dims, DTAG_DIMS, id))
            continue;
        mw = (ULONG)(dims.Nominal.MaxX - dims.Nominal.MinX + 1);
        mh = (ULONG)(dims.Nominal.MaxY - dims.Nominal.MinY + 1);
        if (mw < w || mh < h || (disp.PropertyFlags & DIPF_IS_LACE))
            continue;
        rank = dims.MaxDepth >= 15 && dims.MaxDepth <= 16 ? 3 : dims.MaxDepth > 16 ? 2 : dims.MaxDepth == 8 ? 1 : 0;
        if (!rank)
            continue;
        area = mw * mh;
        if (rank > best_rank || (rank == best_rank && area < best_area)) {
            best_rank = rank;
            best_area = area;
            mode = id;
            depth = rank == 3 ? dims.MaxDepth : rank == 2 ? dims.MaxDepth : 8;
        }
    }
    if (mode == INVALID_ID || !GetDisplayInfoData(NULL, (UBYTE *)&dims, sizeof dims, DTAG_DIMS, mode))
        fail("No screen mode for the full screen picture.\nTry a smaller SCALE or SCREEN=WINDOW.");
    scr = OpenScreenTags(NULL,
                         SA_DisplayID, mode,
                         SA_Width, (ULONG)(dims.Nominal.MaxX - dims.Nominal.MinX + 1),
                         SA_Height, (ULONG)(dims.Nominal.MaxY - dims.Nominal.MinY + 1),
                         SA_Depth, depth,
                         SA_Quiet, TRUE,
                         SA_ShowTitle, FALSE,
                         SA_Type, CUSTOMSCREEN,
                         SA_Title, (ULONG)"Vastar",
                         SA_Colors32, (ULONG)black,
                         TAG_DONE);
    if (!scr)
        fail("Cannot open the screen.");
    if ((ULONG)scr->Width < w || (ULONG)scr->Height < h)
        fail("The screen mode is smaller than the picture.\nTry a smaller SCALE.");
    win = OpenWindowTags(NULL,
                         WA_CustomScreen, (ULONG)scr,
                         WA_Left, 0,
                         WA_Top, 0,
                         WA_Width, (ULONG)scr->Width,
                         WA_Height, (ULONG)scr->Height,
                         /* no title: it would cover the picture (STATS sets one anyway) */
                         WA_Borderless, TRUE,
                         WA_Backdrop, TRUE,
                         WA_Activate, TRUE,
                         WA_RMBTrap, TRUE,
                         WA_SimpleRefresh, TRUE,
                         WA_NoCareRefresh, TRUE,
                         WA_IDCMP, IDCMP_RAWKEY | IDCMP_INACTIVEWINDOW | IDCMP_ACTIVEWINDOW,
                         TAG_DONE);
    if (!win)
        fail("Cannot open the window.");
    blank_pointer = AllocVec(16, MEMF_CHIP | MEMF_CLEAR);
    if (blank_pointer)
        SetPointer(win, blank_pointer, 1, 16, 0, 0);
    SetAPen(win->RPort, 0);
    RectFill(win->RPort, 0, 0, scr->Width - 1, scr->Height - 1);
    ox = (scr->Width - (int)w) / 2;
    oy = (scr->Height - (int)h) / 2;
}

static void open_window(void)
{
    struct ColorMap *cm;
    int i;

    if (screen_full)
        open_fullscreen();
    else
    win = OpenWindowTags(NULL,
                         WA_Title, (ULONG)machine.game->title,
                         WA_InnerWidth, RENDER_W * scale,
                         WA_InnerHeight, RENDER_H * scale,
                         WA_DragBar, TRUE,
                         WA_DepthGadget, TRUE,
                         WA_CloseGadget, TRUE,
                         WA_Activate, TRUE,
                         WA_RMBTrap, TRUE,
                         WA_SmartRefresh, TRUE,
                         WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_RAWKEY | IDCMP_INACTIVEWINDOW |
                                   IDCMP_ACTIVEWINDOW,
                         TAG_DONE);
    if (!win)
        fail("Cannot open the window.");
    if (!screen_full) {
        ox = win->BorderLeft;
        oy = win->BorderTop;
    }

    /* pens are only needed on 8 bit screens (CLUT) and for WriteChunkyPixels */
    cm = win->WScreen->ViewPort.ColorMap;
    for (i = 0; i < 256; i++) {
        ULONG r = video.palette[i][0], g = video.palette[i][1], b = video.palette[i][2];
        pens[i] = ObtainBestPen(cm, r * 0x01010101UL, g * 0x01010101UL, b * 0x01010101UL,
                                OBP_Precision, PRECISION_IMAGE, TAG_DONE);
    }
    pens_ok = 1;

    chunky = AllocVec((ULONG)(RENDER_W * scale * 8 * scale), MEMF_ANY);
    if (!chunky)
        fail("Not enough memory.");
}

/* ------------------------------------------------------------------ RTG video */

/*
 * Picasso96 screens (see "Warp RTG Developer notes" and Wizard of Wor):
 *   BLIT  the changed blocks are written with long moves into an off-screen bitmap
 *         on the board (VRAM is only written) and blitted into the window by the OS.
 *   WPA   a whole frame in fast RAM in the screen format, the changed rectangles go
 *         into the window with p96WritePixelArray.
 *   WCP   no Picasso96: WriteChunkyPixels with pens.
 */
enum { VID_WCP, VID_BLIT, VID_WPA };
static const char *const video_names[] = { "WCP", "BLIT", "WPA" };
static int video_mode = VID_WCP;
static int video_request = -1;
static struct BitMap *vbm;
static UBYTE *wfb;                    /* WPA frame */
static UBYTE *sfb_mem, *sfb;          /* BLIT: one row of blocks in the screen format, 16 byte
                                         aligned (small: it stays in the caches) */
static ULONG sfb_bpr;
static int use_move16, move16_off = 1;   /* MOVE16=ON: see present_rtg */

/* amiga_68k.s: rows from fast RAM to VRAM, writes only (MOVE16 or long moves) */
void copy_rows(const UBYTE *src, UBYTE *dst, ULONG bytes, ULONG rows, ULONG src_mod, ULONG dst_mod,
               ULONG move16);
/* amiga_68k.s: a row of 8x8 blocks of palette indices into the 16 bit frame (scale 1, 2) */
void convert16(const UBYTE *src, UBYTE *dst, const UBYTE *flags, const void *lut, ULONG dst_bpr,
               ULONG scale);
static UWORD native16[256];
static ULONG wfb_bpr, scr_fmt;
static int vbpp;
static ULONG native[256];             /* palette index -> pixel in the screen format */
static ULONG lut[256];                /* the same, doubled for 16 bit at scale 2 */
static ULONG *rowbuf;

#define MAX_RUNS (RENDER_BW * RENDER_BH)
static UBYTE run_by[MAX_RUNS], run_x0[MAX_RUNS], run_x1[MAX_RUNS];
static int run_gap = 3;

static ULONG pixel_value(ULONG fmt, ULONG r, ULONG g, ULONG b, int pen)
{
    ULONG v;
    switch (fmt) {
    case RGBFB_CLUT:      return (ULONG)pen;
    case RGBFB_R5G6B5:    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
    case RGBFB_R5G5B5:    return (r >> 3) << 10 | (g >> 3) << 5 | b >> 3;
    case RGBFB_R5G6B5PC:  v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3; return (v >> 8 | v << 8) & 0xFFFF;
    case RGBFB_R5G5B5PC:  v = (r >> 3) << 10 | (g >> 3) << 5 | b >> 3; return (v >> 8 | v << 8) & 0xFFFF;
    case RGBFB_B5G6R5PC:  v = (b >> 3) << 11 | (g >> 2) << 5 | r >> 3; return (v >> 8 | v << 8) & 0xFFFF;
    case RGBFB_B5G5R5PC:  v = (b >> 3) << 10 | (g >> 3) << 5 | r >> 3; return (v >> 8 | v << 8) & 0xFFFF;
    case RGBFB_A8R8G8B8:  return r << 16 | g << 8 | b;
    case RGBFB_A8B8G8R8:  return b << 16 | g << 8 | r;
    case RGBFB_R8G8B8A8:  return r << 24 | g << 16 | b << 8;
    case RGBFB_B8G8R8A8:  return b << 24 | g << 16 | r << 8;
    default:              return 0;
    }
}

static void open_rtg(void)
{
    struct BitMap *sbm = win->WScreen->RastPort.BitMap;
    ULONG fmt;
    int i, mode;

    if (video_request == VID_WCP)
        return;
    P96Base = OpenLibrary((STRPTR)P96NAME, 2);
    if (!P96Base || !p96GetBitMapAttr(sbm, P96BMA_ISP96))
        return;
    fmt = p96GetBitMapAttr(sbm, P96BMA_RGBFORMAT);
    scr_fmt = fmt;
    vbpp = (int)p96GetBitMapAttr(sbm, P96BMA_BYTESPERPIXEL);
    if (!(vbpp == 1 && fmt == RGBFB_CLUT) && !(vbpp == 2 && pixel_value(fmt, 255, 255, 255, 0)) &&
        !(vbpp == 4 && pixel_value(fmt, 255, 255, 255, 0)))
        return;                                          /* e.g. 24 bit: WriteChunkyPixels */
    rowbuf = AllocVec((ULONG)(RENDER_W * scale * 4 + 16), MEMF_ANY);
    if (!rowbuf)
        return;
    for (i = 0; i < 256; i++) {
        native[i] = pixel_value(fmt, video.palette[i][0], video.palette[i][1], video.palette[i][2],
                                (int)pens[i]);
        lut[i] = vbpp == 2 ? native[i] * 0x00010001UL : native[i];
        native16[i] = (UWORD)native[i];
    }

    mode = video_request >= 0 ? video_request : vbpp == 4 ? VID_WPA : VID_BLIT;
    if (mode == VID_BLIT) {
        vbm = AllocBitMap((ULONG)(RENDER_W * scale), (ULONG)(RENDER_H * scale),
                          GetBitMapAttr(sbm, BMA_DEPTH), BMF_MINPLANES | BMF_DISPLAYABLE, sbm);
        if (vbm && (!p96GetBitMapAttr(vbm, P96BMA_ISP96) || p96GetBitMapAttr(vbm, P96BMA_RGBFORMAT) != fmt)) {
            FreeBitMap(vbm);
            vbm = NULL;
        }
        if (vbm) {
            /* each row of blocks is converted into this buffer in fast RAM and copied to
               VRAM with writes only ("Warp RTG Developer notes"); one row of blocks only,
               a whole frame would not fit in the L2 cache */
            sfb_bpr = ((ULONG)(RENDER_W * scale * vbpp) + 15) & ~15UL;
            sfb_mem = AllocVec(sfb_bpr * (ULONG)(8 * scale) + 16, MEMF_ANY | MEMF_CLEAR);
            if (sfb_mem)
                sfb = (UBYTE *)(((ULONG)sfb_mem + 15) & ~15UL);
            else {
                FreeBitMap(vbm);
                vbm = NULL;
            }
            /* MOVE16 needs a 68040/68060 */
            use_move16 = !move16_off && (((struct ExecBase *)SysBase)->AttnFlags & (AFF_68040 | AFF_68060)) != 0;
        }
        if (!vbm)
            mode = VID_WPA;
    }
    if (mode == VID_WPA) {
        wfb_bpr = (ULONG)(RENDER_W * scale * vbpp);
        wfb = AllocVec(wfb_bpr * (ULONG)(RENDER_H * scale) + 16, MEMF_ANY | MEMF_CLEAR);
        if (!wfb)
            return;
    }
    video_mode = mode;
    if (stats)
        printf("video: format %lu, %d bytes/pixel, using %s\n", fmt, vbpp, video_names[video_mode]);
}

static void close_rtg(void)
{
    if (vbm) {
        WaitBlit();
        FreeBitMap(vbm);
        vbm = NULL;
    }
    if (wfb)
        FreeVec(wfb);
    if (sfb_mem)
        FreeVec(sfb_mem);
    if (rowbuf)
        FreeVec(rowbuf);
    if (P96Base)
        CloseLibrary(P96Base);
}

/* the blocks to send to the window: the ones that changed (the renderer compares),
   or all after the window was activated */
static int find_dirty(UBYTE dirty[RENDER_BH][RENDER_BW])
{
    int bx, by, n = 0;
    for (by = 0; by < RENDER_BH; by++)
        for (bx = 0; bx < RENDER_BW; bx++) {
            UBYTE d = (UBYTE)(force_full || video.blk[by][bx]);
            dirty[by][bx] = d;
            n += d;
        }
    force_full = 0;
    return n;
}

#define LOAD32B(p) (*(const ULONG *)(const void *)(p))

/* BLIT: the blocks of block row `by` marked in flags from indexed into sfb, in the
   screen format (fast RAM) */
static void convert_row(int by, const UBYTE *flags)
{
    const ULONG bw = (ULONG)(8 * scale * vbpp);       /* bytes of a block row */
    int bx, y, x, k;
    if (vbpp == 2 && scale <= 2) {
        convert16(&indexed[by * 8 * RENDER_W], sfb, flags,
                  scale == 1 ? (const void *)native16 : (const void *)lut, sfb_bpr, (ULONG)scale);
        return;
    }
    for (bx = 0; bx < RENDER_BW; bx++) {
        if (!flags[bx])
            continue;
        for (y = 0; y < 8; y++) {
            const uint8_t *src = &indexed[(by * 8 + y) * RENDER_W + bx * 8];
            UBYTE *row = sfb + (ULONG)(y * scale) * sfb_bpr + (ULONG)bx * bw;
            if (vbpp == 4) {
                ULONG *d = (ULONG *)row;
                for (x = 0; x < 8; x++)
                    for (k = 0; k < scale; k++)
                        *d++ = native[src[x]];
            } else if (vbpp == 2) {
                UWORD *d = (UWORD *)row;
                for (x = 0; x < 8; x++)
                    for (k = 0; k < scale; k++)
                        *d++ = (UWORD)native[src[x]];
            } else {
                UBYTE *d = row;
                for (x = 0; x < 8; x++)
                    for (k = 0; k < scale; k++)
                        *d++ = (UBYTE)native[src[x]];
            }
            for (k = 1; k < scale; k++)
                CopyMem(row, row + (ULONG)k * sfb_bpr, bw);
        }
    }
}

/* the dirty blocks as runs per block row; a run may bridge up to run_gap clean blocks */
static int collect_runs(UBYTE dirty[RENDER_BH][RENDER_BW])
{
    int by, n = 0;
    for (by = 0; by < RENDER_BH; by++) {
        int bx = 0;
        while (bx < RENDER_BW) {
            int run1;
            if (!dirty[by][bx]) {
                if ((bx & 3) == 0 && !*(const ULONG *)&dirty[by][bx])
                    bx += 4;                     /* RENDER_BW is a multiple of 4 */
                else
                    bx++;
                continue;
            }
            run_by[n] = (UBYTE)by;
            run_x0[n] = (UBYTE)bx;
            run1 = bx + 1;
            for (bx = run1; bx < RENDER_BW && bx < run1 + 1 + run_gap; bx++)
                if (dirty[by][bx])
                    run1 = bx + 1;
            run_x1[n++] = (UBYTE)run1;
            bx = run1;
        }
    }
    return n;
}

/* the rows of all runs in the screen format into a buffer (VRAM: long writes only) */
static void write_runs(UBYTE *base, ULONG bpr, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        const int y0 = run_by[i] * 8, x0 = run_x0[i] * 8, pixels = (run_x1[i] - run_x0[i]) * 8;
        int y, x, k;
        for (y = y0; y < y0 + 8; y++) {
            const uint8_t *src = &indexed[y * RENDER_W + x0];
            UBYTE *row = base + (ULONG)(y * scale) * bpr + (ULONG)x0 * scale * vbpp;
            if (vbpp == 2 && scale == 2) {           /* one long = one doubled pixel */
                ULONG *d0 = (ULONG *)row, *d1 = (ULONG *)(row + bpr);
                for (x = 0; x < pixels; x++)
                    d0[x] = d1[x] = lut[src[x]];
                continue;
            }
            if (vbpp == 4 && scale == 1) {
                ULONG *d = (ULONG *)row;
                for (x = 0; x < pixels; x++)
                    d[x] = lut[src[x]];
                continue;
            }
            /* general case: build the row in rowbuf, copy it scale times */
            {
                ULONG bytes = (ULONG)pixels * scale * vbpp;
                if (vbpp == 4) {
                    ULONG *d = rowbuf;
                    for (x = 0; x < pixels; x++)
                        for (k = 0; k < scale; k++)
                            *d++ = lut[src[x]];
                } else if (vbpp == 2) {
                    UWORD *d = (UWORD *)rowbuf;
                    for (x = 0; x < pixels; x++)
                        for (k = 0; k < scale; k++)
                            *d++ = (UWORD)native[src[x]];
                } else {
                    UBYTE *d = (UBYTE *)rowbuf;
                    for (x = 0; x < pixels; x++)
                        for (k = 0; k < scale; k++)
                            *d++ = (UBYTE)native[src[x]];
                }
                for (k = 0; k < scale; k++) {
                    UBYTE *dst = row + (ULONG)k * bpr;
                    if (((ULONG)dst & 3) == 0 && (bytes & 3) == 0)
                        CopyMemQuick(rowbuf, dst, bytes);
                    else
                        CopyMem(rowbuf, dst, bytes);
                }
            }
        }
    }
}

static void present_rtg(int n, UBYTE dirty[RENDER_BH][RENDER_BW])
{
    int i, by;
    if (video_mode == VID_WPA) {
        struct RenderInfo ri;
        write_runs(wfb, wfb_bpr, n);
        ri.Memory = wfb;
        ri.BytesPerRow = (WORD)wfb_bpr;
        ri.pad = 0;
        ri.RGBFormat = (RGBFTYPE)scr_fmt;
        for (i = 0; i < n; i++) {
            const int y0 = run_by[i] * 8, x0 = run_x0[i] * 8, x1 = run_x1[i] * 8;
            p96WritePixelArray(&ri, (UWORD)(x0 * scale), (UWORD)(y0 * scale), win->RPort,
                               (UWORD)(ox + x0 * scale), (UWORD)(oy + y0 * scale),
                               (UWORD)((x1 - x0) * scale), (UWORD)(8 * scale));
        }
        return;
    }
    {
        /* per block row: the span from the first to the last changed block is converted
           into sfb and copied to VRAM, 16 byte aligned, with writes only */
        struct RenderInfo ri;
        LONG lock = p96LockBitMap(vbm, (UBYTE *)&ri, sizeof ri);
        if (ri.Memory) {
            const ULONG bpr = (UWORD)ri.BytesPerRow, bw = (ULONG)(8 * scale * vbpp);
            const ULONG m16 = use_move16 && !((ULONG)ri.Memory & 15) && !(bpr & 15);
            for (by = 0; by < RENDER_BH; by++) {
                int bx = 0;
                if (!LOAD32B(dirty[by]) && !LOAD32B(dirty[by] + 4) && !LOAD32B(dirty[by] + 8) &&
                    !LOAD32B(dirty[by] + 12) && !LOAD32B(dirty[by] + 16) && !LOAD32B(dirty[by] + 20) &&
                    !LOAD32B(dirty[by] + 24))
                    continue;
                convert_row(by, dirty[by]);
                if (m16)   /* MOVE16 reads memory, not the copyback data cache */
                    CacheClearE(sfb, (ULONG)(8 * scale) * sfb_bpr, CACRF_ClearD);
                /* each run of changed blocks (16 byte lines, whole runs only: what
                   lies between them was not converted) */
                while (bx < RENDER_BW) {
                    int x0;
                    ULONG b0, b1;
                    if (!dirty[by][bx]) {
                        bx++;
                        continue;
                    }
                    x0 = bx;
                    while (bx < RENDER_BW && dirty[by][bx])
                        bx++;
                    b0 = (ULONG)x0 * bw;
                    b1 = (ULONG)bx * bw;
                    copy_rows(sfb + b0, (UBYTE *)ri.Memory + (ULONG)(by * 8 * scale) * bpr + b0,
                              b1 - b0, (ULONG)(8 * scale), sfb_bpr, bpr, m16 && !(b0 & 15) && !(b1 & 15));
                }
            }
        }
        p96UnlockBitMap(vbm, lock);
        if (stats > 1)
            prof_stage(5);               /* write */
        if (!ri.Memory) {
            force_full = 1;
            return;
        }
    }
    /* the off-screen bitmap holds the whole picture: one blit of the rectangle around
       all changed blocks (blits from VRAM are done by the board, system calls cost) */
    {
        int x0 = RENDER_BW, x1 = -1, y0 = RENDER_BH, y1 = -1, bx;
        for (by = 0; by < RENDER_BH; by++)
            for (bx = 0; bx < RENDER_BW; bx++)
                if (dirty[by][bx]) {
                    if (bx < x0) x0 = bx;
                    if (bx > x1) x1 = bx;
                    if (by < y0) y0 = by;
                    y1 = by;
                }
        if (x1 >= 0)
            BltBitMapRastPort(vbm, (WORD)(x0 * 8 * scale), (WORD)(y0 * 8 * scale), win->RPort,
                              (WORD)(ox + x0 * 8 * scale), (WORD)(oy + y0 * 8 * scale),
                              (WORD)((x1 + 1 - x0) * 8 * scale), (WORD)((y1 + 1 - y0) * 8 * scale), 0xC0);
    }
    (void)i;
}

/* without Picasso96: WriteChunkyPixels with pens */
static void present_chunky(int n)
{
    int i, y, x, k;
    for (i = 0; i < n; i++) {
        const int y0 = run_by[i] * 8, x0 = run_x0[i] * 8, pixels = (run_x1[i] - run_x0[i]) * 8;
        const int w = pixels * scale;
        for (y = 0; y < 8; y++) {
            const uint8_t *src = &indexed[(y0 + y) * RENDER_W + x0];
            UBYTE *line = &chunky[y * scale * w], *dst = line;
            for (x = 0; x < pixels; x++)
                for (k = 0; k < scale; k++)
                    *dst++ = (UBYTE)pens[src[x]];
            for (k = 1; k < scale; k++)
                memcpy(line + k * w, line, (size_t)w);
        }
        WriteChunkyPixels(win->RPort,
                          (ULONG)(ox + x0 * scale), (ULONG)(oy + y0 * scale),
                          (ULONG)(ox + (x0 + pixels) * scale - 1),
                          (ULONG)(oy + (y0 + 8) * scale - 1),
                          chunky, (LONG)w);
    }
}

static ULONG prof_blocks;
static uint64_t stat_stage[7], stage_t0;

/* VERIFY (testing): every 30th drawn frame read the window back from the board and
   compare every block with what it should show; the result goes to the title */
static int verify;
static ULONG verify_bad, verify_checks, verify_frames;
static int verify_bx = -1, verify_by;

static void verify_window(void)
{
    static UBYTE *buf;
    const int w = RENDER_W * scale, h = RENDER_H * scale;
    struct RenderInfo ri;
    int bx, by;

    if (!buf)
        buf = AllocVec((ULONG)(w * h * vbpp), MEMF_ANY);
    if (!buf || !P96Base || video_mode == VID_WCP)
        return;
    WaitBlit();
    ri.Memory = buf;
    ri.BytesPerRow = (WORD)(w * vbpp);
    ri.pad = 0;
    ri.RGBFormat = (RGBFTYPE)scr_fmt;
    p96ReadPixelArray(&ri, 0, 0, win->RPort, (UWORD)ox, (UWORD)oy,
                      (UWORD)w, (UWORD)h);
    verify_checks++;
    for (by = 0; by < RENDER_BH; by++)
        for (bx = 0; bx < RENDER_BW; bx++) {
            int y, x, bad = 0;
            for (y = by * 8; y < by * 8 + 8 && !bad; y++)
                for (x = bx * 8; x < bx * 8 + 8; x++) {
                    const UBYTE *row = buf + (ULONG)(y * scale) * ri.BytesPerRow;
                    ULONG want = native[indexed[y * RENDER_W + x]], got;
                    if (vbpp == 2) {
                        got = ((const UWORD *)row)[x * scale];
                        want &= 0xFFFF;
                    } else if (vbpp == 4) {
                        got = ((const ULONG *)row)[x * scale] & 0xFFFFFF;
                        want &= 0xFFFFFF;
                    } else {
                        got = row[x * scale];
                        want &= 0xFF;
                    }
                    if (got != want) {
                        bad = 1;
                        break;
                    }
                }
            if (bad) {
                verify_bad++;
                verify_bx = bx;
                verify_by = by;
            }
        }
}

/* STATS=FULL: time of the stages of a frame (renderer 0-3, window 4-6) */
static void prof_stage(int stage)
{
    uint64_t t = eclock_now();
    stat_stage[stage] += t - stage_t0;
    stage_t0 = t;
}

static void present(void)
{
    static UBYTE dirty[RENDER_BH][RENDER_BW];
    int n;
    if (stats > 1)
        stage_t0 = eclock_now();
    prof_blocks += (ULONG)find_dirty(dirty);
    if (video_mode == VID_BLIT) {        /* write exactly the changed blocks */
        int gap = run_gap;
        run_gap = 0;
        n = collect_runs(dirty);
        run_gap = gap;
    } else {
        n = collect_runs(dirty);
    }
    if (stats > 1)
        prof_stage(4);                   /* compare */
    if (!n)
        return;
    if (video_mode == VID_WCP)
        present_chunky(n);
    else
        present_rtg(n, dirty);
    if (stats > 1)
        prof_stage(6);                   /* blit (or all of WPA / WCP) */
    if (verify && ++verify_frames % 30 == 0)
        verify_window();
}

/* ------------------------------------------------------------------ timing */

static void open_timer(void)
{
    struct EClockVal ev;

    timer_port = CreateMsgPort();
    if (!timer_port)
        fail("Cannot create a message port.");
    timer_req = (struct timerequest *)CreateIORequest(timer_port, sizeof(struct timerequest));
    if (!timer_req || OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)timer_req, 0)) {
        if (timer_req)
            DeleteIORequest((struct IORequest *)timer_req);
        timer_req = NULL;
        fail("Cannot open timer.device.");
    }
    TimerBase = timer_req->tr_node.io_Device;
    eclock_freq = ReadEClock(&ev);
}

static uint64_t eclock_now(void)
{
    struct EClockVal ev;
    ReadEClock(&ev);
    return ((uint64_t)ev.ev_hi << 32) | ev.ev_lo;
}

/* wait until the next arcade frame (60.606 Hz) is due; returns 0 if it is overdue by
   more than a third of a frame (a little lateness evens out by itself and is no
   reason to leave out pictures) */
static int pace(void)
{
    const uint64_t period = (uint64_t)eclock_freq * 1000 / VASTAR_FPS_X1000;
    uint64_t now = eclock_now();

    if (next_frame == 0 || now > next_frame + eclock_freq / 4)
        next_frame = now;                                /* start or resync after a stall */
    next_frame += period;
    if (next_frame > now) {
        ULONG micros = (ULONG)((next_frame - now) * 1000000 / eclock_freq);
        if (micros > 1000) {
            timer_req->tr_node.io_Command = TR_ADDREQUEST;
            timer_req->tr_time.tv_secs = 0;
            timer_req->tr_time.tv_micro = micros;
            DoIO((struct IORequest *)timer_req);
        }
        return 1;
    }
    return now - next_frame <= period / 3;
}

/* ------------------------------------------------------------------ sound */

/*
 * AHI library, double buffered (see Wizard of Wor): AHI plays two dynamic sounds
 * in turn; when one starts, the SoundFunc hook queues the other and the game
 * refills it from a ring that the AY emulation fills once per frame.
 */
#define SND_BUF 768                   /* 35 ms at 22050 Hz */
#define SND_RING 8192

static struct MsgPort *ahi_port;
static struct AHIRequest *ahi_io;
static struct AHIAudioCtrl *actrl;
static int16_t *snd_buf[2];
static struct Task *snd_task;
static BYTE snd_signal = -1;
static volatile UWORD snd_queued;
static volatile UWORD snd_to_fill;
static volatile ULONG snd_swaps;
static ULONG snd_handled;
static int16_t snd_ring[SND_RING];
static ULONG ring_r, ring_w;
enum { SND_NONE, SND_AHI, SND_PAULA };
static const char *const sound_names[] = { "no sound", "AHI", "Paula" };
static int sound_request = SND_PAULA;   /* Paula: no mixing on the CPU; SOUND=AHI */
static int sound_mode = SND_NONE;

static ULONG sound_func(__reg("a0") struct Hook *hook, __reg("a2") struct AHIAudioCtrl *ac,
                        __reg("a1") struct AHISoundMessage *msg)
{
    (void)hook;
    (void)msg;
    snd_queued ^= 1;
    AHI_SetSound(0, snd_queued, 0, 0, ac, 0);
    snd_to_fill = snd_queued;
    snd_swaps++;
    Signal(snd_task, 1UL << snd_signal);
    return 0;
}

static struct Hook sound_hook = { {NULL, NULL}, (HOOKFUNC)sound_func, NULL, NULL };

static void open_ahi(void)
{
    struct AHISampleInfo info;
    int i;

    ahi_port = CreateMsgPort();
    if (!ahi_port)
        return;
    ahi_io = (struct AHIRequest *)CreateIORequest(ahi_port, sizeof(struct AHIRequest));
    if (!ahi_io)
        return;
    ahi_io->ahir_Version = 4;
    if (OpenDevice((STRPTR)AHINAME, AHI_NO_UNIT, (struct IORequest *)ahi_io, 0)) {
        DeleteIORequest((struct IORequest *)ahi_io);
        ahi_io = NULL;
        return;
    }
    AHIBase = (struct Library *)ahi_io->ahir_Std.io_Device;

    snd_task = FindTask(NULL);
    snd_signal = AllocSignal(-1);
    for (i = 0; i < 2; i++)
        snd_buf[i] = AllocVec(SND_BUF * sizeof(int16_t), MEMF_PUBLIC | MEMF_CLEAR);
    if (snd_signal < 0 || !snd_buf[0] || !snd_buf[1])
        return;

    actrl = AHI_AllocAudio(AHIA_AudioID, AHI_DEFAULT_ID,
                           AHIA_MixFreq, AHI_DEFAULT_FREQ,
                           AHIA_Channels, 1,
                           AHIA_Sounds, 2,
                           AHIA_SoundFunc, (ULONG)&sound_hook,
                           TAG_DONE);
    if (!actrl)
        return;
    for (i = 0; i < 2; i++) {
        info.ahisi_Type = AHIST_M16S;
        info.ahisi_Address = snd_buf[i];
        info.ahisi_Length = SND_BUF;
        if (AHI_LoadSound((UWORD)i, AHIST_DYNAMICSAMPLE, &info, actrl) != AHIE_OK) {
            AHI_FreeAudio(actrl);
            actrl = NULL;
            return;
        }
    }
    if (AHI_ControlAudio(actrl, AHIC_Play, TRUE, TAG_DONE) != AHIE_OK) {
        AHI_FreeAudio(actrl);
        actrl = NULL;
        return;
    }
    snd_queued = 1;
    AHI_Play(actrl,
             AHIP_BeginChannel, 0,
             AHIP_Freq, AUDIO_RATE,
             AHIP_Vol, 0x10000,
             AHIP_Pan, 0x8000,
             AHIP_Sound, 0,
             AHIP_EndChannel, NULL,
             TAG_DONE);
}

/* Paula through audio.device: one left and one right channel play the same 8 bit buffers */
#define PAULA_BUFS 3

static struct MsgPort *aud_port;
static struct IOAudio *aud_alloc;
static struct IOAudio *aud_io[PAULA_BUFS][2];
static BYTE *aud_buf[PAULA_BUFS];
static UBYTE aud_busy[PAULA_BUFS];

static void paula_send(int b, ULONG n)
{
    int c;
    for (c = 0; c < 2; c++) {
        aud_io[b][c]->ioa_Request.io_Command = CMD_WRITE;
        aud_io[b][c]->ioa_Request.io_Flags = ADIOF_PERVOL;
        aud_io[b][c]->ioa_Length = n;
        BeginIO((struct IORequest *)aud_io[b][c]);
    }
    aud_busy[b] = 2;
}

static int open_paula(void)
{
    static UBYTE channels[] = { 3, 5, 10, 12 };
    const ULONG clock = (GfxBase->DisplayFlags & PAL) ? 3546895 : 3579545;
    ULONG units;
    int b, c;

    aud_port = CreateMsgPort();
    if (!aud_port)
        return 0;
    aud_alloc = (struct IOAudio *)CreateIORequest(aud_port, sizeof(struct IOAudio));
    if (!aud_alloc)
        return 0;
    aud_alloc->ioa_Request.io_Message.mn_Node.ln_Pri = 1;
    aud_alloc->ioa_Request.io_Flags = ADIOF_NOWAIT;
    aud_alloc->ioa_Data = channels;
    aud_alloc->ioa_Length = sizeof channels;
    if (OpenDevice((STRPTR)AUDIONAME, 0, (struct IORequest *)aud_alloc, 0)) {
        DeleteIORequest((struct IORequest *)aud_alloc);
        aud_alloc = NULL;
        return 0;
    }
    units = (ULONG)aud_alloc->ioa_Request.io_Unit;
    for (b = 0; b < PAULA_BUFS; b++) {
        aud_buf[b] = AllocMem(SND_BUF, MEMF_CHIP | MEMF_CLEAR);
        if (!aud_buf[b])
            return 0;
        for (c = 0; c < 2; c++) {
            struct IOAudio *io = AllocMem(sizeof *io, MEMF_PUBLIC);
            if (!io)
                return 0;
            *io = *aud_alloc;
            io->ioa_Request.io_Unit = (struct Unit *)(units & (c ? 0x06 : 0x09));
            io->ioa_Data = (UBYTE *)aud_buf[b];
            io->ioa_Period = (UWORD)(clock / AUDIO_RATE);
            io->ioa_Volume = 64;
            io->ioa_Cycles = 1;
            aud_io[b][c] = io;
        }
    }
    for (b = 0; b < PAULA_BUFS; b++)
        paula_send(b, 64);
    return 1;
}

static void close_paula(void)
{
    int b, c;

    for (b = 0; b < PAULA_BUFS; b++)
        for (c = 0; c < 2; c++)
            if (aud_io[b][c]) {
                if (aud_busy[b]) {
                    AbortIO((struct IORequest *)aud_io[b][c]);
                    WaitIO((struct IORequest *)aud_io[b][c]);
                }
                FreeMem(aud_io[b][c], sizeof(struct IOAudio));
                aud_io[b][c] = NULL;
            }
    if (aud_alloc) {
        CloseDevice((struct IORequest *)aud_alloc);
        DeleteIORequest((struct IORequest *)aud_alloc);
        aud_alloc = NULL;
    }
    for (b = 0; b < PAULA_BUFS; b++)
        if (aud_buf[b]) {
            FreeMem(aud_buf[b], SND_BUF);
            aud_buf[b] = NULL;
        }
    if (aud_port) {
        DeleteMsgPort(aud_port);
        aud_port = NULL;
    }
}

static void paula_push(void)
{
    struct IOAudio *io;
    int b, c;

    while ((io = (struct IOAudio *)GetMsg(aud_port)))
        for (b = 0; b < PAULA_BUFS; b++)
            for (c = 0; c < 2; c++)
                if (aud_io[b][c] == io && aud_busy[b])
                    aud_busy[b]--;
    if (ring_w - ring_r > 4 * SND_BUF)
        ring_r = ring_w - 2 * SND_BUF;
    for (b = 0; b < PAULA_BUFS; b++) {
        ULONG avail = ring_w - ring_r, n, i;
        if (aud_busy[b] || avail < 2)
            continue;
        n = (avail < SND_BUF ? avail : SND_BUF) & ~1UL;
        for (i = 0; i < n; i++)
            aud_buf[b][i] = (BYTE)(snd_ring[ring_r++ % SND_RING] >> 8);
        paula_send(b, n);
    }
}

/* the chosen one first (Paula by default: AHI mixes in software, which costs about 5%
   of a 68060/50), the other one if it cannot be had (channels in use, no AHI) */
static void open_sound(void)
{
    if (sound_request == SND_AHI) {
        open_ahi();
        if (actrl) {
            sound_mode = SND_AHI;
            return;
        }
    }
    if (open_paula()) {
        sound_mode = SND_PAULA;
        return;
    }
    close_paula();
    if (sound_request != SND_AHI) {
        open_ahi();
        if (actrl)
            sound_mode = SND_AHI;
    }
}

static void close_sound(void)
{
    close_paula();
    if (actrl) {
        AHI_ControlAudio(actrl, AHIC_Play, FALSE, TAG_DONE);
        AHI_FreeAudio(actrl);
        actrl = NULL;
    }
    if (ahi_io) {
        CloseDevice((struct IORequest *)ahi_io);
        DeleteIORequest((struct IORequest *)ahi_io);
    }
    if (ahi_port)
        DeleteMsgPort(ahi_port);
    if (snd_buf[0])
        FreeVec(snd_buf[0]);
    if (snd_buf[1])
        FreeVec(snd_buf[1]);
    if (snd_signal >= 0)
        FreeSignal(snd_signal);
}

static void audio_push(const int16_t *samples, int count)
{
    int i;

    if (sound_mode == SND_NONE)
        return;
    for (i = 0; i < count; i++)
        snd_ring[ring_w++ % SND_RING] = samples[i];
    if (ring_w - ring_r > SND_RING - SND_BUF)
        ring_r = ring_w - 2 * SND_BUF;

    if (sound_mode == SND_PAULA) {
        paula_push();
        return;
    }
    if (snd_swaps != snd_handled) {
        int16_t *dst = snd_buf[snd_to_fill];
        ULONG avail = ring_w - ring_r, n = avail < SND_BUF ? avail : SND_BUF;
        snd_handled = snd_swaps;
        SetSignal(0, 1UL << snd_signal);
        if (avail > 4 * SND_BUF) {
            ring_r = ring_w - 2 * SND_BUF;
            n = SND_BUF;
        }
        for (i = 0; i < (int)n; i++)
            dst[i] = snd_ring[ring_r++ % SND_RING];
        for (; i < SND_BUF; i++)
            dst[i] = 0;
    }
}

/* ------------------------------------------------------------------ input */

#define RAW_1      0x01
#define RAW_2      0x02
#define RAW_5      0x05
#define RAW_6      0x06
#define RAW_9      0x09
#define RAW_P      0x19
#define RAW_Z      0x31
#define RAW_X      0x32
#define RAW_SPACE  0x40
#define RAW_ESC    0x45
#define RAW_UP     0x4C
#define RAW_DOWN   0x4D
#define RAW_RIGHT  0x4E
#define RAW_LEFT   0x4F
#define RAW_F3     0x52
#define RAW_CTRL   0x63
#define RAW_LALT   0x64
#define RAW_RALT   0x65

static void handle_messages(void)
{
    struct IntuiMessage *msg;
    int quit = 0;

    while ((msg = (struct IntuiMessage *)GetMsg(win->UserPort))) {
        ULONG cls = msg->Class;
        UWORD code = msg->Code;
        ReplyMsg((struct Message *)msg);
        if (cls == IDCMP_CLOSEWINDOW)
            quit = 1;
        else if (cls == IDCMP_RAWKEY) {
            int down = !(code & IECODE_UP_PREFIX);
            keys[code & 0x7F] = (UBYTE)down;
            if (code == RAW_ESC)
                quit = 1;
            else if (code == RAW_P)
                paused = !paused;
            else if (code == RAW_F3)
                vastar_reset(&machine);
        } else if (cls == IDCMP_INACTIVEWINDOW)
            memset(keys, 0, sizeof keys);
        else if (cls == IDCMP_ACTIVEWINDOW)
            force_full = 1;                              /* repaint everything once */
    }
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
        quit = 1;
    if (quit)
        exit(0);
}

/* the arcade inputs from the keyboard and the joystick; ReadJoyPort() is slow on
   real hardware, so it is read once per frame */
static void read_inputs(void)
{
    uint8_t p1 = 0, sys = 0;

    if (keys[RAW_LEFT]) p1 |= IN_LEFT;
    if (keys[RAW_RIGHT]) p1 |= IN_RIGHT;
    if (keys[RAW_UP]) p1 |= IN_UP;
    if (keys[RAW_DOWN]) p1 |= IN_DOWN;
    if (keys[RAW_CTRL] || keys[RAW_SPACE] || keys[RAW_Z]) p1 |= IN_BUTTON1;
    if (keys[RAW_LALT] || keys[RAW_RALT] || keys[RAW_X]) p1 |= IN_BUTTON2;
    if (keys[RAW_5]) sys |= IN_COIN1;
    if (keys[RAW_6]) sys |= IN_COIN2;
    if (keys[RAW_9]) sys |= IN_SERVICE;
    if (keys[RAW_1]) sys |= IN_START1;
    if (keys[RAW_2]) sys |= IN_START2;

    /* the joystick port, and with JOYPORT=BOTH the mouse port too (a mouse there is
       ignored). Joystick: fire, second button = button 2. CD32 pad: red = fire,
       blue = button 2, play = start, green = coin, yellow = start 2 players.
       FREEPLAY: fire also starts a game (harmless during one). */
    if (LowLevelBase) {
        int port;
        for (port = 1; port >= (joy_mouseport ? 0 : 1); port--) {
            ULONG j = ReadJoyPort((ULONG)port), type = j & JP_TYPE_MASK;
            if (port == 1)
                joy_state = j;
            if (type != JP_TYPE_JOYSTK && type != JP_TYPE_GAMECTLR)
                continue;
            if (j & JPF_JOY_UP) p1 |= IN_UP;
            if (j & JPF_JOY_DOWN) p1 |= IN_DOWN;
            if (j & JPF_JOY_LEFT) p1 |= IN_LEFT;
            if (j & JPF_JOY_RIGHT) p1 |= IN_RIGHT;
            if (j & JPF_BUTTON_RED) {
                p1 |= IN_BUTTON1;
                if (freeplay)
                    sys |= IN_START1;
            }
            if (j & JPF_BUTTON_BLUE) p1 |= IN_BUTTON2;
            if (type == JP_TYPE_GAMECTLR) {
                if (j & JPF_BUTTON_PLAY) sys |= IN_START1;
                if (j & JPF_BUTTON_GREEN) sys |= IN_COIN1;
                if (j & JPF_BUTTON_YELLOW) sys |= IN_START2;
            }
        }
    }
    machine.in_p1 = p1;
    machine.in_p2 = 0;
    machine.in_system = sys;
}

/* ------------------------------------------------------------------ options */

static int same_text(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y)
            return 0;
    }
    return *a == *b;
}

static void parse_option(const char *opt)
{
    static char rp[4][256], sn[32];
    char name[12];
    const char *val;
    int i;

    for (i = 0; i < 11 && opt[i] && opt[i] != '='; i++)
        name[i] = opt[i];
    name[i] = 0;
    val = opt[i] == '=' ? opt + i + 1 : 0;

    if (same_text(name, "SCALE") && val)
        scale = atoi(val);
    else if (same_text(opt, "NOSOUND"))
        sound = 0;
    else if (same_text(opt, "STATS"))
        stats = 1;
    else if (same_text(opt, "STATS=FULL"))
        stats = 2;
    else if (same_text(opt, "SOUND=AHI"))
        sound_request = SND_AHI;
    else if (same_text(opt, "SOUND=PAULA"))
        sound_request = SND_PAULA;
    else if (same_text(opt, "NORTG") || same_text(opt, "VIDEO=WCP"))
        video_request = VID_WCP;
    else if (same_text(opt, "VIDEO=BLIT"))
        video_request = VID_BLIT;
    else if (same_text(opt, "VIDEO=WPA"))
        video_request = VID_WPA;
    else if (same_text(opt, "VERIFY"))
        verify = 1;                                      /* testing */
    else if (same_text(opt, "MOVE16=ON"))
        move16_off = 0;                                  /* testing */
    else if (same_text(opt, "FRAMESKIP=AUTO"))
        frameskip = -1;
    else if (same_text(name, "FRAMESKIP") && val)
        frameskip = atoi(val);
    else if (same_text(opt, "LIVES=INFINITE"))
        infinite_lives = 1;
    else if (same_text(name, "LIVES") && val)
        lives = atoi(val);
    else if (same_text(opt, "FREEPLAY"))
        freeplay = 1;
    else if (same_text(opt, "P2=SEPARATE"))
        p2separate = 1;
    else if (same_text(opt, "JOYPORT=BOTH"))
        joy_mouseport = 1;
    else if (same_text(opt, "SCREEN=FULL"))
        screen_full = 1;
    else if (same_text(opt, "SCREEN=WINDOW"))
        screen_full = 0;
    else if (same_text(name, "DSW1") && val)
        dsw1 = (int)strtol(val, 0, 16);
    else if (same_text(name, "DSW2") && val)
        dsw2 = (int)strtol(val, 0, 16);
    else if (same_text(name, "ROMPATH") && val && nrompaths < 4) {
        snprintf(rp[nrompaths], sizeof rp[0], "%s", val);
        rompaths[nrompaths] = rp[nrompaths];
        nrompaths++;
    } else if ((same_text(name, "SET") && val) || !val) {
        snprintf(sn, sizeof sn, "%s", val ? val : opt);
        for (i = 0; sn[i]; i++)
            if (sn[i] >= 'A' && sn[i] <= 'Z')
                sn[i] = (char)(sn[i] + 32);
        set_name = sn;
    }
}

static void parse_tool_types(struct WBStartup *wb)
{
    struct Library *IconBase = OpenLibrary((STRPTR)"icon.library", 36);
    struct DiskObject *dob;
    BPTR old;

    if (!IconBase)
        return;
    old = CurrentDir(wb->sm_ArgList[0].wa_Lock);
    dob = GetDiskObject(wb->sm_ArgList[0].wa_Name);
    if (dob) {
        STRPTR *tt;
        for (tt = dob->do_ToolTypes; tt && *tt; tt++)
            if (**tt != '(')
                parse_option((const char *)*tt);
        FreeDiskObject(dob);
    }
    CurrentDir(old);
    CloseLibrary(IconBase);
}

/* ------------------------------------------------------------------ ROMs */

static void load_roms(void)
{
    static char paths[32][256];
    static char msg[1024];
    static const char *const dirs[] = { "PROGDIR:", "PROGDIR:rom", "PROGDIR:roms" };
    const char *src[32], *all[8];
    const GameDef *game = vastar_find_game(set_name);
    int n = 0, d, i, ndirs = 0, len;

    if (!game) {
        len = sprintf(msg, "Unknown set: %s\nKnown sets:", set_name);
        for (i = 0; i < vastar_game_count; i++)
            len += sprintf(msg + len, " %s", vastar_games[i].name);
        fail(msg);
    }
    for (d = 0; d < nrompaths; d++)
        all[ndirs++] = rompaths[d];
    for (d = 0; d < 3; d++)
        all[ndirs++] = dirs[d];
    for (d = 0; d < ndirs; d++) {
        const char *names[2];
        names[0] = game->name;
        names[1] = game->parent;
        for (i = 0; i < 2; i++) {
            char file[40];
            if (!names[i] || n + 2 > 32)
                continue;
            sprintf(file, "%s.zip", names[i]);
            path_join(paths[n], 256, all[d], file);
            src[n] = paths[n];
            n++;
            path_join(paths[n], 256, all[d], names[i]);
            src[n] = paths[n];
            n++;
        }
        if (n < 32) {
            snprintf(paths[n], 256, "%s", all[d]);
            src[n] = paths[n];
            n++;
        }
    }
    if (vastar_load_roms(&machine, game, src, n)) {
        len = sprintf(msg, "Cannot load the ROMs of %s.\n\nPut %s.zip or the ROM files into\n"
                           "the program drawer, its rom drawer\nor give ROMPATH=dir.", game->title, game->name);
        fail(msg);
    }
}

/* ------------------------------------------------------------------ main */

static uint64_t stat_start, stat_emu, stat_video, stat_present, stat_busy;

static int stat_frames;
static char stat_title[200];

static void update_stats(uint64_t t_end)
{
    uint64_t elapsed;
    if (!stat_start)
        stat_start = t_end;
    if (++stat_frames < 121)
        return;
    elapsed = t_end - stat_start;
    if (!elapsed)
        elapsed = 1;
    {
        ULONG fps10 = (ULONG)((uint64_t)stat_frames * eclock_freq * 10 / elapsed);
        ULONG busy = (ULONG)(stat_busy * 100 / elapsed);
        if (stats > 1)
            sprintf(stat_title, "Vastar %lu.%lu fps (drawn %lu) CPU %lu%%: emu %lu video %lu [t%lu m%lu l%lu s%lu] present(%s) %lu [c%lu w%lu b%lu] blocks %lu %s",
                    fps10 / 10, fps10 % 10, (ULONG)((uint64_t)stat_drawn * eclock_freq / elapsed), busy,
                    (ULONG)(stat_emu * 100 / elapsed),
                    (ULONG)(stat_video * 100 / elapsed), (ULONG)(stat_stage[0] * 100 / elapsed),
                    (ULONG)(stat_stage[1] * 100 / elapsed), (ULONG)(stat_stage[2] * 100 / elapsed),
                    (ULONG)(stat_stage[3] * 100 / elapsed),
                    video_names[video_mode],
                    (ULONG)(stat_present * 100 / elapsed), (ULONG)(stat_stage[4] * 100 / elapsed),
                    (ULONG)(stat_stage[5] * 100 / elapsed), (ULONG)(stat_stage[6] * 100 / elapsed),
                    prof_blocks / (ULONG)stat_frames,
                    sound_names[sound_mode]);
        else
            sprintf(stat_title, "Vastar %lu.%lu fps (drawn %lu)  CPU %lu%%  %s", fps10 / 10, fps10 % 10,
                    (ULONG)((uint64_t)stat_drawn * eclock_freq / elapsed), busy, sound_names[sound_mode]);
    }
    if (verify)
        sprintf(stat_title + strlen(stat_title), "  verify: %lu bad blocks in %lu checks (last %d,%d)",
                verify_bad, verify_checks, verify_bx, verify_by);
    SetWindowTitles(win, (UBYTE *)stat_title, (UBYTE *)~0);
    stat_frames = 0;
    stat_emu = stat_video = stat_present = stat_busy = 0;
    stat_drawn = 0;
    memset(stat_stage, 0, sizeof stat_stage);
    prof_blocks = 0;
    stat_start = t_end;
}

int main(int argc, char **argv)
{
    int i;

    if (argc == 0)
        parse_tool_types((struct WBStartup *)argv);
    for (i = 1; i < argc; i++)
        parse_option(argv[i]);
    if (scale < 1 || scale > 3)
        scale = 2;

    atexit(cleanup);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 40);
    if (!IntuitionBase || !GfxBase)
        fail("Needs AmigaOS 3.1 or newer.");
    LowLevelBase = OpenLibrary((STRPTR)"lowlevel.library", 40);   /* optional */

    load_roms();
    vastar_init(&machine, AUDIO_RATE);
    if (dsw1 >= 0) machine.dsw1 = (uint8_t)dsw1;
    if (dsw2 >= 0) machine.dsw2 = (uint8_t)dsw2;
    if (freeplay)
        machine.dsw2 &= 0xf8;            /* coin A: free play */
    if (lives > 9)
        lives = 9;                       /* more icons than fit in the bottom row otherwise */
    machine.lives = (uint8_t)(lives > 0 ? lives : 0);
    machine.infinite_lives = (uint8_t)infinite_lives;
    machine.p2_from_p1 = (uint8_t)!p2separate;   /* by default player 2 uses the same joystick */
    video_init(&video, &machine);

    open_window();
    open_rtg();
    open_timer();
    if (sound)
        open_sound();
    start_watchdog();

    for (;;) {
        static int late, skipped;
        uint64_t t0 = 0, t1, t2, t3;
        int draw;
        if (stats)
            t0 = eclock_now();
        read_inputs();
        /* frame skipping: the game always runs at full speed, the picture is left out
           when the last frame finished too late (at most MAX_AUTO_SKIP in a row) */
        if (frameskip < 0)
            draw = !late || skipped >= MAX_AUTO_SKIP;
        else
            draw = skipped >= frameskip;
        if (!paused) {
            int n = vastar_run_frame(&machine, sound_mode != SND_NONE ? audio : 0);
            if (stats) {
                t1 = eclock_now();
                stat_emu += t1 - t0;
            }
            audio_push(audio, n);
            if (!draw) {
                skipped++;
                if (stats)
                    stat_busy += eclock_now() - t0;
                goto frame_done;
            }
            skipped = 0;
            stat_drawn++;
            if (stats > 1) {
                video_prof = prof_stage;
                stage_t0 = eclock_now();
            }
            video_render_screen(&video, &machine, indexed);
            if (stats) {
                t2 = eclock_now();
                stat_video += t2 - t1;
            }
            present();
            if (stats) {
                t3 = eclock_now();
                stat_present += t3 - t2;
                stat_busy += t3 - t0;
            }
        }
    frame_done:
        diag_frames++;
        handle_messages();
        late = !pace();
        if (stats)
            update_stats(eclock_now());
    }
    return 0;
}
