#!/bin/sh
# The release of the Vastar port (after make -f Makefile.amiga and make in port/).
# The number comes from the $VER string in port/src/main_amiga.c ("$VER: Vastar 0.1 (...)").
#   releases/amiga/Vastar/                 the public Amiga release, in git: Vastar, Vastar.info,
#                                          doc/ + doc.info (descriptions PL/EN, dev PL/EN,
#                                          release history PL/EN), empty rom/ with ReadMe.txt
#   releases/amiga/Vastar.info             the game drawer's icon (WARP logo)
#   releases/amiga/Vastar-<v>.zip          the same as an archive - no ROMs
#   releases/pc/Vastar/                    the public PC release: vastar.exe, SDL2.dll,
#                                          doc/ (descriptions PL/EN, SDL2 licence), rom/ReadMe.txt
#   releases/pc/Vastar-<v>-pc.zip          the same as an archive - no ROMs
#   private/Vastar-<v>-full.zip            + the ROMs (rom/) in Vastar/rom: only on this PC
#   private/Vastar-<v>-pc-full.zip         (private/ is not in git; older ones -> private/old)
# On the Amiga:  UnZip Vastar-<v>.zip -d Work:Games
#   tools/make_release.sh [--no-full]
set -e
cd "$(dirname "$0")/.."
V=$(sed -n 's/.*"\$VER: Vastar \([0-9][0-9.]*\) .*/\1/p' port/src/main_amiga.c)
[ -n "$V" ] || { echo "no \$VER: Vastar <v> in port/src/main_amiga.c" >&2; exit 1; }
A=releases/amiga
P=releases/pc

grep -a -q "\$VER: Vastar $V " build/amiga/Vastar || { echo "build/amiga/Vastar is not $V: make -f Makefile.amiga first" >&2; exit 1; }
# the PC program has no version string: only checked to be newer than its sources
for s in port/src/*.c port/src/*.h; do
	[ "$s" = port/src/main_amiga.c ] && continue
	[ "$s" -nt build/port/vastar.exe ] && echo "warning: $s is newer than build/port/vastar.exe" >&2
done

# the Amiga tree
mkdir -p "$A/Vastar/doc" "$A/Vastar/rom"
cp build/amiga/Vastar "$A/Vastar/Vastar"
cp port/amiga/Vastar.info "$A/Vastar/Vastar.info"
cp port/amiga/Vastar.txt port/amiga/Vastar_en.txt port/amiga/Vastar_dev.txt port/amiga/Vastar_dev_en.txt \
   port/amiga/Vastar_release.txt port/amiga/Vastar_release_en.txt "$A/Vastar/doc/"
cp port/amiga/rom_ReadMe.txt "$A/Vastar/rom/ReadMe.txt"
python tools/make_doc_icon.py "$A/Vastar/doc.info" --frame tools/icon/drawer.png --crop 0 0 46 40 --transparent --drawer >/dev/null
cp tools/icon/warp-drawer.info "$A/Vastar.info"	# the game drawer: the WARP logo (tools/icon/warp-drawer.info, ../WARP-drawer.info)

# the PC tree (texts with CR LF for Windows)
mkdir -p "$P/Vastar/doc" "$P/Vastar/rom"
cp build/port/vastar.exe build/port/SDL2.dll "$P/Vastar/"
for t in Vastar.txt Vastar_en.txt SDL2-LICENSE.txt; do
	sed 's/\r*$/\r/' "port/pc/$t" > "$P/Vastar/doc/$t"
done
sed 's/\r*$/\r/' port/pc/rom_ReadMe.txt > "$P/Vastar/rom/ReadMe.txt"

# never ROMs in the public trees
python tools/check_no_roms.py rom "$A/Vastar" "$P/Vastar"

# the public archives (only the current version; older ones out of git and to private/old)
mkdir -p private/old
for f in "$A"/Vastar-*.zip "$A"/Vastar-*.lha "$P"/Vastar-*.zip; do
	case "$f" in "$A/Vastar-$V.zip"|"$P/Vastar-$V-pc.zip") continue;; esac
	[ -e "$f" ] && { git rm -q --cached "$f" 2>/dev/null || true; mv "$f" private/old/; }
done
python tools/make_zip.py "$A/Vastar-$V.zip" "$A" Vastar Vastar.info
python tools/make_zip.py "$P/Vastar-$V-pc.zip" "$P" Vastar
python tools/check_no_roms.py rom "$A/Vastar-$V.zip" "$P/Vastar-$V-pc.zip"

if [ "$1" != "--no-full" ]; then
	# only the current full archives in private/, the older ones in private/old (kept)
	for f in private/Vastar-*-full.zip; do
		case "$f" in "private/Vastar-$V-full.zip"|"private/Vastar-$V-pc-full.zip") continue;; esac
		[ -e "$f" ] && mv "$f" private/old/
	done
	python tools/make_zip.py "private/Vastar-$V-full.zip" "$A" Vastar Vastar.info --add rom Vastar/rom
	python tools/make_zip.py "private/Vastar-$V-pc-full.zip" "$P" Vastar --add rom Vastar/rom
fi
ls -l "$A/Vastar-$V.zip" "$P/Vastar-$V-pc.zip" private/Vastar-$V-*full.zip 2>/dev/null
