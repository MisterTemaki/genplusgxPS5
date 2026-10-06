#!/bin/bash
# Genesis Plus GX PS5 host tests: the real port code (main-boot, shims, frontend, Genesis Plus GX's core; the installer and the
# helper) on Linux, with the PS5 calls faked by host/sce_host.cpp. A scripted pad drives the app; flips are
# de-tiled to PPM and checked. Each system gets a tiny test ROM (tests/make_test_rom.py): red backdrop, green
# while Cross is held.
set -u
cd "$(dirname "$0")/.."
BIN=$PWD/build/host/genplus-ps5-host
INSTALLER=$PWD/build/host/genplus-ps5-installer
HELPER=$PWD/build/host/genplus-ps5-helper
# ports nobody listens on, so the app's ordinary runs find no helper and no ELF loader
export GENPLUS_HELPER_PORT=$((20000 + RANDOM % 10000)) GENPLUS_ELFLDR_PORT=$((30000 + RANDOM % 10000)) GENPLUS_JB_NO_OTHERS=1
CHECK="python3 $PWD/tests/check_ppm.py"
RECT="python3 $PWD/tests/check_rect.py"
ROM="python3 $PWD/tests/make_test_rom.py"
WORK=${WORK:-$(mktemp -d)}
ONLY=${ONLY:-}
PASS=0
FAIL=0

# pad bits
CROSS=4000; CIRCLE=2000; TRIANGLE=1000; UP=10; DOWN=40; RIGHT=20; L2=100; R2=200; L3R3=6; OPTIONS=8
L2UP=$(printf %x $((0x$L2 | 0x$UP))); L2DOWN=$(printf %x $((0x$L2 | 0x$DOWN))); L2R2=$(printf %x $((0x$L2 | 0x$R2)))
# pause menu: L3+R3, then Up from "Resume" wraps to "Quit Genesis Plus GX PS5"
QUITAT() { echo "$1:$L3R3;$(($1 + 2)):0;$(($1 + 10)):$UP;$(($1 + 12)):0;$(($1 + 20)):$CROSS;$(($1 + 22)):0"; }

ok() { echo "  ok: $*"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $*"; FAIL=$((FAIL + 1)); }
expect() { if eval "$1"; then ok "$2"; else bad "$2"; fi; }
want() { [ -z "$ONLY" ] || [[ " $ONLY " == *" $1 "* ]]; }

newroot() {
	local t=$WORK/$1
	rm -rf "$t" && mkdir -p "$t/root/roms" "$t/dump"
	echo "$t"
}

run() { # dir pad dumps [args...]
	local t=$1 pad=$2 dumps=$3
	shift 3
	GENPLUS_HOST_OFFLINE=${OFFLINE-1} GENPLUS_COVER_URL=${COVER_URL-} \
	GENPLUS_PS5_ROOT=$t/root GENPLUS_PS5_HOMEBREW=$t/homebrew GENPLUS_HOST_DUMP_DIR=$t/dump GENPLUS_HOST_DUMP=$dumps \
		GENPLUS_HOST_MAX_FLIPS=${MAXFLIPS-5000} GENPLUS_HOST_PAD=$pad ASAN_OPTIONS=detect_leaks=0 timeout 180 "$BIN" "$@" >"$t/out.txt" 2>&1
	echo $?
}
nosan() { expect "! grep -q 'runtime error\|AddressSanitizer' $1/out.txt" "no sanitizer reports"; }

# A game for each system: picture where the log says, Cross -> green, quit from the pause menu.
system_test() { # name ext label
	local name=$1 ext=$2 label=$3
	local T
	T=$(newroot "$name")
	$ROM "$T/root/roms/test.$ext" ${4:-ntsc} >/dev/null
	local rc
	rc=$(run "$T" "0:0;100:$CROSS;140:0;$(QUITAT 200)" "90,130" "$T/root/roms/test.$ext")
	expect "[ $rc = 0 ]" "$label: exit code 0 (got $rc)"
	expect "grep -q 'running \"test\" ($label)' $T/root/logs/boot.log" "$label: Genesis Plus GX runs it"
	expect "$RECT $T/dump/flip00090.ppm $T/root/logs/boot.log red >/dev/null" "$label: red picture where the log says, black around it"
	expect "$CHECK $T/dump/flip00130.ppm 960 540 green >/dev/null" "$label: Cross -> green"
	nosan "$T"
}

if want 1; then
echo "== 1. Mega Drive from the command line: picture, input, quick save (L2 + Up), quit from the pause menu"
T=$(newroot t1)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
rc=$(run "$T" "0:0;100:$CROSS;140:0;200:$L2UP;205:0;$(QUITAT 220)" "90,130" "$T/root/roms/test.md")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "$RECT $T/dump/flip00090.ppm $T/root/logs/boot.log red >/dev/null" "red picture where the log says, black around it"
expect "$CHECK $T/dump/flip00090.ppm 960 540 red >/dev/null" "the Mega Drive backdrop is red"
expect "$CHECK $T/dump/flip00130.ppm 960 540 green >/dev/null" "Cross -> Mega Drive B -> green"
expect "[ -f $T/root/states/test.state1 ]" "L2 + Up wrote states/test.state1"
expect "grep -q 'running \"test\" (Mega Drive), 59.9' $T/root/logs/boot.log" "a 60 Hz Mega Drive game"
expect "grep -q 'save state 1: ok' $T/root/logs/boot.log" "logged the save"
expect "grep -q 'Fit to screen' $T/root/logs/boot.log" "fit to screen by default"
expect "grep -q '\[core\] ' $T/root/logs/boot.log" "the core's own log reaches boot.log"
nosan "$T"
fi

if want 2; then
echo "== 2. the other systems"
system_test t2sms sms "Master System"
system_test t2gg gg "Game Gear"
system_test t2gen gen "Mega Drive"
fi

if want 3; then
echo "== 3. shelf -> game -> back to the shelf -> quit"
T=$(newroot t3)
$ROM "$T/root/roms/Test Game (USA).md" ntsc >/dev/null
mkdir -p "$T/root/roms/Action"
rc=$(run "$T" "0:0;40:$CROSS;42:0;150:$L3R3;152:0;160:$UP;162:0;164:$UP;166:0;170:$CROSS;172:0;200:$OPTIONS;202:0;210:$CROSS;212:0" "20,35,120,190")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "[ -f $T/dump/flip00020.ppm ]" "the shelf was shown"
expect "$CHECK $T/dump/flip00120.ppm 960 540 red >/dev/null" "the game picked on the shelf runs"
expect "grep -q 'last_rom=.*Test Game (USA).md' $T/root/genplus-ps5.ini" "the shelf remembers the last game"
expect "grep -q 'game closed' $T/root/logs/boot.log" "back to the list closed the game"
nosan "$T"
fi

if want 4; then
echo "== 4. a 50 Hz (PAL) game runs at 50 frames a second"
T=$(newroot t4)
$ROM "$T/root/roms/pal.md" pal >/dev/null
start=$(date +%s.%N)
rc=$(run "$T" "0:0;$(QUITAT 300)" "" "$T/root/roms/pal.md")
end=$(date +%s.%N)
secs=$(python3 -c "print(round($end - $start, 2))")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "python3 -c 'import sys; sys.exit(0 if 5.0 <= $secs <= 9.5 else 1)'" "300 frames took ${secs}s (~6 s at 50 fps)"
expect "grep -q 'running \"pal\" (Mega Drive), 49.[0-9]' $T/root/logs/boot.log" "the core runs the European game at 50 Hz"
fi

if want 5; then
echo "== 5. Sega CD without its BIOS: the screen names the files; the tracks of a .cue are not games of their own"
T=$(newroot t5)
mkdir -p "$T/root/roms/SegaCD" "$T/root/roms/MegaDrive"
head -c $((2352 * 300)) /dev/zero >"$T/root/roms/SegaCD/Game (USA) (Track 1).bin"
head -c $((2352 * 100)) /dev/zero >"$T/root/roms/SegaCD/Game (USA) (Track 2).bin"
cat >"$T/root/roms/SegaCD/Game (USA).cue" <<'CUE'
FILE "Game (USA) (Track 1).bin" BINARY
  TRACK 01 MODE1/2352
    INDEX 01 00:00:00
FILE "Game (USA) (Track 2).bin" BINARY
  TRACK 02 AUDIO
    INDEX 00 00:00:00
    INDEX 01 00:02:00
CUE
$ROM "$T/root/roms/MegaDrive/Alpha (USA).md" ntsc >/dev/null
# the shelf: Alpha, then Game; Right -> Game, Cross -> the BIOS message, Cross -> back, Options -> quit
rc=$(run "$T" "0:0;30:$RIGHT;32:0;50:$CROSS;52:0;90:$CROSS;92:0;110:$OPTIONS;112:0;120:$CROSS;122:0" "70")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '\[games\] 2 ROM(s)' $T/root/logs/boot.log" "the shelf lists the .cue and the .md, not the tracks"
expect "grep -q 'missing firmware: Sega CD BIOS' $T/root/logs/boot.log" "logged: the Sega CD BIOS is missing"
expect "grep -q 'could not start .*Game (USA).cue: missing the Sega CD BIOS' $T/root/logs/boot.log" "the game wasn't started"
expect "[ -f $T/dump/flip00070.ppm ]" "a message was on the screen"
mkdir -p "$T/root/bios" && head -c 131072 /dev/zero >"$T/root/bios/bios_CD_U.bin"
rc=$(GENPLUS_HOST_QUIT_AFTER=3 run "$T" "0:0" "" "$T/root/roms/SegaCD/Game (USA).cue")
expect "grep -q 'loading .*Game (USA).cue' $T/root/logs/boot.log && ! grep -q 'missing firmware' $T/root/logs/boot.log" "with bios/bios_CD_U.bin the disc goes to the core"
nosan "$T"
fi

if want 6; then
echo "== 6. integer scale and scanlines from the settings file; load state with L2 + Down"
T=$(newroot t6)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
printf 'scale=1\nscanlines=1\n' >"$T/root/genplus-ps5.ini"
rc=$(run "$T" "0:0;60:$L2UP;62:0;80:$L2DOWN;82:0;$(QUITAT 100)" "50" "$T/root/roms/test.md")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'Integer scale, scanlines' $T/root/logs/boot.log" "integer scale with scanlines"
expect "$RECT $T/dump/flip00050.ppm $T/root/logs/boot.log red >/dev/null" "red picture where the log says"
expect "python3 - $T/dump/flip00050.ppm <<'PY'
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3)
w, h = map(int, d[1].split()); px = d[3]
reds = {px[(y * w + 960) * 3] for y in range(100, 980)}
top = max(reds)
sys.exit(0 if top >= 200 and any(0.35 * top <= r <= 0.65 * top for r in reds) else 1)
PY" "scanline rows are darker"
expect "grep -q 'load state 1: ok' $T/root/logs/boot.log" "L2 + Down loaded the state"
nosan "$T"
fi

if want 7; then
echo "== 7. a zipped game; the sound stays fed and near the 60 ms target"
T=$(newroot t7)
$ROM "$T/game.md" ntsc >/dev/null
python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1],'w',zipfile.ZIP_DEFLATED); z.write(sys.argv[2],'game.md'); z.close()" "$T/root/roms/game.zip" "$T/game.md"
rc=$(GENPLUS_HOST_REALTIME=1 run "$T" "0:0;$(QUITAT 760)" "100" "$T/root/roms/game.zip")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'loading .*game.zip -> game.md' $T/root/logs/boot.log" "the game inside the zip is loaded"
expect "$CHECK $T/dump/flip00100.ppm 960 540 red >/dev/null" "the zipped game runs"
u2=$(grep -o 'audio queued .*underruns [0-9]*' "$T/root/logs/boot.log" | sed -n 2p | grep -o '[0-9]*$')
u3=$(grep -o 'audio queued .*underruns [0-9]*' "$T/root/logs/boot.log" | sed -n 3p | grep -o '[0-9]*$')
lat=$(grep -o 'audio queued [0-9]* ([0-9]* ms)' "$T/root/logs/boot.log" | sed -n 3p | grep -o '([0-9]*' | tr -d '(')
under=$(( ${u3:-999} - ${u2:-0} ))
expect "[ $under -le 2 ]" "audio underruns over 5 s of play: $under"
expect "[ ${lat:-0} -ge 30 ] && [ ${lat:-0} -le 120 ]" "audio latency ${lat:-?} ms"
nosan "$T"
fi

if want 8; then
echo "== 8. little video memory: 720p scan-out; memory busy at first: retries"
T=$(newroot t8)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
rc=$(GENPLUS_HOST_DIRECT_MAX_MIB=12 run "$T" "0:0;$(QUITAT 100)" "90" "$T/root/roms/test.md")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'scan-out 1280x720' $T/root/logs/boot.log" "fell back to 1280x720"
expect "head -2 $T/dump/flip00090.ppm | grep -q '1280 720'" "the flips are 1280x720"
expect "$RECT $T/dump/flip00090.ppm $T/root/logs/boot.log red >/dev/null" "game picture in place (720p)"
T=$(newroot t8b)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
rc=$(GENPLUS_HOST_DIRECT_FAIL_FIRST=9 run "$T" "0:0;$(QUITAT 100)" "" "$T/root/roms/test.md")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'retrying (1)' $T/root/logs/boot.log && grep -q 'scan-out 1920x1080' $T/root/logs/boot.log" "retried, then got 1080p"
fi

if want 9; then
echo "== 9. GenesisPlusGXPS5.elf installs the app (icon and libc.prx included), stays as the helper, repairs"
T=$(newroot t9)
APP=$T/homebrew/PPSA99011
inst() { # dir -> runs the installer; it stays running as the helper when the port is free
	GENPLUS_PS5_ROOT=$1/root GENPLUS_PS5_HOMEBREW=$1/homebrew GENPLUS_PS5_APPMETA=$1/appmeta ASAN_OPTIONS=detect_leaks=0 timeout 60 "$INSTALLER" >>"$1/inst.txt" 2>&1
}
waitfor() { for i in $(seq 1 50); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
stop_helpers() { pkill -f 'build/host/genplus-ps5-(installer|helper)' 2>/dev/null; pkill -f 'received.elf' 2>/dev/null; sleep 0.3; }
stop_helpers
inst "$T" &
expect "waitfor $T/root/logs/installer.log 'listening on 127.0.0.1'" "the installer stays running as the helper"
expect "cmp -s $APP/sce_sys/icon0.png app/sce_sys/icon0.png" "icon0.png installed (the Genesis Plus GX PS5 icon)"
expect "cmp -s $APP/sce_sys/pic0.dds app/sce_sys/pic0.dds && cmp -s $APP/sce_sys/pic1.dds app/sce_sys/pic1.dds && cmp -s $APP/sce_sys/param.json app/sce_sys/param.json" "pic0.dds/pic1.dds (background) and param.json installed"
expect "cmp -s $APP/eboot.bin tests/fake-eboot.bin" "eboot.bin installed"
expect "cmp -s $APP/sce_module/libc.prx tests/fake-libc.prx" "sce_module/libc.prx installed"
expect "grep -q 'Genesis Plus GX PS5 [0-9.]* installed. Open it from the Genesis Plus GX PS5 icon' $T/inst.txt" "install notification says to open the icon"
expect "! ls $APP/*.part $APP/sce_sys/*.part $APP/sce_module/*.part 2>/dev/null | grep -q ." "no .part files left"
inst "$T"
expect "grep -q 'is up to date' $T/root/logs/installer.log && ! grep -q 'wrote' $T/root/logs/installer.log" "sent again: nothing rewritten"
expect "grep -q 'a helper is already running' $T/root/logs/installer.log" "sent again: the second copy leaves the helper to the first"
echo broken > $APP/sce_sys/icon0.png
inst "$T"
expect "cmp -s $APP/sce_sys/icon0.png app/sce_sys/icon0.png" "a damaged icon is put back"
META=$T/appmeta/PPSA99011
mkdir -p "$META" && echo old > "$META/pic0.png" && echo old > "$META/icon0.png" && cp app/sce_sys/param.json "$META/"
echo old > "$APP/sce_sys/pic0.png"
inst "$T"
expect "cmp -s $META/pic0.dds app/sce_sys/pic0.dds && cmp -s $META/pic1.dds app/sce_sys/pic1.dds && cmp -s $META/icon0.png app/sce_sys/icon0.png" "appmeta gets the background (pic0/pic1.dds) and icon"
expect "[ ! -f $META/pic0.png ] && [ ! -f $APP/sce_sys/pic0.png ]" "an old pic0.png is removed"
expect "grep -q 'Home screen art updated' $T/inst.txt" "the notification says the home screen art changed"
stop_helpers
expect "[ \$(find $T -path '*PPSA99009*' -o -path '*PPSA99010*' -o -path '*PPSA99203*' | wc -l) = 0 ]" "nothing written for Snes9x PS5 (PPSA99009), Mesen2 PS5 (PPSA99010) or PS5SX2 (PPSA99203)"
fi

if want 10; then
echo "== 10. covers per system: download, git-symlink, name by CRC, your own cover first, 404 remembered"
T=$(newroot t10)
MDREPO=Sega_-_Mega_Drive_-_Genesis; SMSREPO=Sega_-_Master_System_-_Mark_III
mkdir -p "$T/srv/$MDREPO/Named_Boxarts" "$T/srv/$SMSREPO/Named_Boxarts" "$T/root/covers/MegaDrive" "$T/root/roms/MegaDrive" "$T/root/roms/MasterSystem"
python3 - "$T/srv" "$T/root/covers" "$MDREPO" "$SMSREPO" <<'PY'
import sys
from PIL import Image
srv, covers, md, sms = sys.argv[1:5]
m, s = f"{srv}/{md}/Named_Boxarts", f"{srv}/{sms}/Named_Boxarts"
Image.new('RGB', (512, 357), (255, 0, 0)).save(m + '/Sonic The Hedgehog (USA, Europe).png')
Image.new('RGB', (512, 357), (0, 255, 0)).save(m + '/Streets of Rage 2 (Europe).png')
open(m + '/Streets of Rage 2 (USA).png', 'w').write('Streets of Rage 2 (Europe).png')
Image.new('RGB', (512, 357), (255, 255, 0)).save(m + '/Golden Axe (World).png')
Image.new('RGB', (360, 512), (0, 0, 255)).save(covers + '/MegaDrive/Golden Axe (World).png')
Image.new('RGB', (360, 512), (255, 0, 255)).save(s + '/Alex Kidd in Miracle World (USA, Europe).png')
PY
for n in "Sonic The Hedgehog (USA, Europe)" "Comix Zone (USA)" "Golden Axe (World)"; do $ROM "$T/root/roms/MegaDrive/$n.md" ntsc >/dev/null; done
$ROM "$T/root/roms/MasterSystem/Alex Kidd in Miracle World (USA, Europe).sms" >/dev/null
$ROM "$T/root/roms/MegaDrive/sor2.md" ntsc >/dev/null
python3 tests/forge_crc.py "$T/root/roms/MegaDrive/sor2.md" $(grep -P "^md\t[0-9A-F]{8}\tStreets of Rage 2 \(USA\)$" frontend/data/nointro.tsv | cut -f2) >/dev/null
PORT=18080
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
sleep 1
# shelf order: Alex Kidd, Comix Zone, Golden Axe, Sonic The Hedgehog, Streets of Rage 2
rc=$(OFFLINE= COVER_URL="http://127.0.0.1:$PORT/\${repo}/Named_Boxarts/\${name}.png" GENPLUS_HOST_REALTIME=1 run "$T" \
	"0:0;180:$RIGHT;182:0;230:$RIGHT;232:0;280:$RIGHT;282:0;330:$RIGHT;332:0;380:$OPTIONS;382:0;390:$CROSS;392:0" "170,220,270,320,370")
kill $SRVPID 2>/dev/null
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'sor2.md \[md\] -> \"Streets of Rage 2 (USA)\" (by CRC)' $T/root/logs/boot.log" "sor2.md named by its CRC"
expect "[ -f '$T/root/covers/MegaDrive/Sonic The Hedgehog (USA, Europe).png' ]" "a Mega Drive cover saved in covers/MegaDrive"
expect "[ -f '$T/root/covers/MasterSystem/Alex Kidd in Miracle World (USA, Europe).png' ]" "a Master System cover from its repository, in covers/MasterSystem"
expect "[ -f '$T/root/covers/MegaDrive/Comix Zone (USA).missing' ]" "a 404 is remembered (.missing)"
expect "grep -q 'is a link to Streets of Rage 2 (Europe).png' $T/root/logs/boot.log" "a git-symlink cover is followed"
expect "! grep -q 'GET .*Golden%20Axe' $T/root/logs/boot.log" "your own cover (covers/MegaDrive/<ROM name>.png): nothing downloaded for it"
expect "$CHECK $T/dump/flip00170.ppm 960 420 255 0 255 >/dev/null" "Alex Kidd shows the Master System (magenta) cover"
expect "! $CHECK $T/dump/flip00220.ppm 960 420 255 0 0 >/dev/null 2>&1 && ! $CHECK $T/dump/flip00220.ppm 960 420 0 255 0 >/dev/null 2>&1" "no cover online: a placeholder card"
expect "$CHECK $T/dump/flip00270.ppm 960 420 0 0 255 >/dev/null" "Golden Axe shows your own (blue) cover, not the server's"
expect "$CHECK $T/dump/flip00320.ppm 960 420 255 0 0 >/dev/null" "Sonic shows the downloaded (red) cover"
expect "$CHECK $T/dump/flip00370.ppm 960 420 0 255 0 >/dev/null" "Streets of Rage 2 shows the linked (green) cover"
nosan "$T"
fi

if want 11; then
echo "== 11. offline: no download is tried, the shelf still works"
T=$(newroot t11)
$ROM "$T/root/roms/Sonic The Hedgehog (USA, Europe).md" ntsc >/dev/null
rc=$(run "$T" "0:0;30:$OPTIONS;32:0;40:$CROSS;42:0" "20")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'not connected' $T/root/logs/boot.log && ! grep -q 'GET ' $T/root/logs/boot.log" "no request made without a network"
fi

if want 12; then
echo "== 12. the shelf's tabs: Down shows one system; the game picked there runs"
T=$(newroot t12)
$ROM "$T/root/roms/Alpha (USA).sms" >/dev/null
$ROM "$T/root/roms/Beta (USA).md" ntsc >/dev/null
$ROM "$T/root/roms/Gamma (USA).gg" >/dev/null
rc=$(run "$T" "0:0;30:$DOWN;32:0;60:$CROSS;62:0;$(QUITAT 160)" "25,55,150")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '\[shelf\] tab Mega Drive: 1 game(s)' $T/root/logs/boot.log" "Down: the Mega Drive tab, one game"
expect "grep -q 'loading .*Beta (USA).md' $T/root/logs/boot.log" "the Mega Drive game was started from its tab"
expect "grep -q 'shelf_family=1' $T/root/genplus-ps5.ini" "the tab is remembered"
expect "$CHECK $T/dump/flip00150.ppm 960 540 red >/dev/null" "the Mega Drive game runs"
nosan "$T"
fi

if want 13; then
echo "== 13. settings from the shelf (Triangle); fast forward (R2) and rewind (L2 + R2); the pause menu saves a state"
T=$(newroot t13)
$ROM "$T/root/roms/Alpha (USA).md" ntsc >/dev/null
# Triangle -> settings ("Screen size" first), Right -> integer scale, Circle -> back; Cross -> play
rc=$(GENPLUS_HOST_REALTIME=1 run "$T" "0:0;30:$TRIANGLE;32:0;40:$RIGHT;42:0;50:$CIRCLE;52:0;70:$CROSS;72:0;150:$R2;250:0;300:$L2R2;340:0;360:$L3R3;362:0;370:$DOWN;372:0;380:$CROSS;382:0;400:$CIRCLE;402:0;$(QUITAT 440)" "45")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'scale=1' $T/root/genplus-ps5.ini" "the settings screen changed the screen size"
expect "grep -q 'fast forward on' $T/root/logs/boot.log && grep -q 'fast forward off' $T/root/logs/boot.log" "R2 held: fast forward"
expect "grep -q 'rewind on' $T/root/logs/boot.log && grep -q 'rewind off' $T/root/logs/boot.log" "L2 + R2 held: rewind"
expect "[ -f '$T/root/states/Alpha (USA).state1' ]" "the pause menu's Save state wrote slot 1"
nosan "$T"
fi

if want 14; then
echo "== 14. the pad is shared with the system (handle 0x809b0081, as on the console)"
T=$(newroot t14)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
rc=$(GENPLUS_HOST_PAD_SHARED=1 GENPLUS_HOST_REALTIME=1 run "$T" "0:0;100:$CROSS;140:0;$(QUITAT 300)" "130" "$T/root/roms/test.md")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'using 809b0081 (shared with the system)' $T/root/logs/boot.log" "the system's handle is used"
expect "[ \$(grep -c 'scePadOpen' $T/root/logs/boot.log) = 1 ]" "opened once, not every 2 seconds"
expect "$CHECK $T/dump/flip00130.ppm 960 540 green >/dev/null" "its buttons reach the game (Cross -> green)"
fi

if want 15; then
echo "== 15. the app asks the helper to let it out of the sandbox"
waitfor() { for i in $(seq 1 50); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
stop_helpers() { pkill -f 'build/host/genplus-ps5-(installer|helper)' 2>/dev/null; pkill -f 'received.elf' 2>/dev/null; sleep 0.3; }
T=$(newroot t15)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
QUIT=$(QUITAT 30)
GENPLUS_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/hroot/logs/helper.log" "listening" || sleep 1
rc=$(run "$T" "$QUIT" "" "$T/root/roms/test.md")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'Genesis Plus GX helper (port [0-9]*): ret 0' $T/root/logs/boot.log" "the helper said yes"
expect "grep -q 'jailbreak: Genesis Plus GX helper' $T/root/logs/boot.log" "logged before /data was open, written to boot.log"
expect "grep -q 'letting it out' $T/hroot/logs/helper.log" "the helper let the app's process out"
stop_helpers
T=$(newroot t15b)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
GENPLUS_HOST_JB_TITLE=PPSA99009 GENPLUS_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/hroot/logs/helper.log" "listening" || sleep 1
rc=$(run "$T" "$QUIT" "" "$T/root/roms/test.md")
expect "grep -q 'title PPSA99009 is not Genesis Plus GX PS5' $T/hroot/logs/helper.log" "the helper lets out no other title (not even Snes9x PS5)"
stop_helpers

echo "== 16. no helper running: the app hands its own helper to the ELF loader, then asks it"
T=$(newroot t16)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
python3 - "$GENPLUS_ELFLDR_PORT" "$T" <<'PY' &
import os, socket, subprocess, sys
port, t = int(sys.argv[1]), sys.argv[2]
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port)); s.listen(1); s.settimeout(60)
c, _ = s.accept()
data = b''
while True:
    d = c.recv(65536)
    if not d: break
    data += d
path = t + '/received.elf'
open(path, 'wb').write(data); os.chmod(path, 0o755)
env = dict(os.environ, GENPLUS_PS5_ROOT=t + '/hroot', ASAN_OPTIONS='detect_leaks=0')
p = subprocess.Popen(['timeout', '30', path], env=env, stdout=open(t + '/helper.txt', 'w'), stderr=subprocess.STDOUT)
open(t + '/helper.pid', 'w').write(str(p.pid))
PY
LPID=$!
sleep 0.5
rc=$(run "$T" "$QUIT" "" "$T/root/roms/test.md")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "cmp -s $T/received.elf $HELPER" "the ELF loader got the helper built into the app"
expect "grep -q 'jailbreak: Genesis Plus GX helper (started by the app)' $T/root/logs/boot.log" "then the helper it started let it out"
wait $LPID 2>/dev/null
stop_helpers
fi

if want 17; then
echo "== 17. no /data even so: the screen says what to do; the screen busy at first: retries"
T=$(newroot t17)
rm -rf "$T/root" && echo "not a folder" >"$T/root" # the app can't create its folders there
rc=$(run "$T" "0:0;30:$CROSS;32:0" "20")
expect "[ $rc = 2 ]" "exit code 2 (got $rc)"
expect "grep -q 'no access to /data' $T/out.txt" "logged: no access to /data"
expect "[ -f $T/dump/flip00020.ppm ]" "a message was on the screen"
T=$(newroot t17b)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
rc=$(GENPLUS_HOST_VIDEO_BUSY=3 run "$T" "$(QUITAT 30)" "" "$T/root/roms/test.md")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "[ \$(grep -c 'sceVideoOutOpen -> .*80290009' $T/root/logs/boot.log) = 3 ] && grep -q 'scan-out 1920x1080' $T/root/logs/boot.log" "VideoOut busy three times, then opened"
expect "grep -q 'splash screen hidden' $T/root/logs/boot.log" "the splash screen is hidden"
fi

if want 18; then
echo "== 18. covers as PS5SX2: prefetched before asking for /data; new games restart the app to fetch them"
waitfor() { for i in $(seq 1 50); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
stop_helpers() { pkill -f 'build/host/genplus-ps5-(installer|helper)' 2>/dev/null; pkill -f 'received.elf' 2>/dev/null; sleep 0.3; }
stop_helpers
T=$(newroot t18)
SRV=$T/srv/Sega_-_Mega_Drive_-_Genesis/Named_Boxarts; mkdir -p "$SRV" "$T/root/covers"
python3 -c "from PIL import Image; Image.new('RGB',(512,357),(255,0,0)).save('$SRV/Sonic The Hedgehog (USA, Europe).png')"
$ROM "$T/root/roms/Sonic The Hedgehog (USA, Europe).md" ntsc >/dev/null
PORT=18081
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
GENPLUS_PS5_ROOT=$T/root ASAN_OPTIONS=detect_leaks=0 timeout 120 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/root/logs/helper.log" "listening" || sleep 1
URL="http://127.0.0.1:$PORT/\${repo}/Named_Boxarts/\${name}.png"
SHELFQUIT="0:0;30:$OPTIONS;32:0;40:$CROSS;42:0"
rc=$(OFFLINE= COVER_URL="$URL" run "$T" "$SHELFQUIT" "")
expect "grep -q 'MegaDrive/Sonic The Hedgehog (USA, Europe).png' $T/root/covers/wanted.txt" "first start: the missing cover goes to covers/wanted.txt"
expect "grep -q 'restarting .* so the prefetch gets them' $T/root/logs/boot*.log" "first start: the app restarts itself for the new cover"
expect "[ ! -f '$T/root/covers/MegaDrive/Sonic The Hedgehog (USA, Europe).png' ]" "first start: nothing downloaded on the shelf (as PS5SX2)"
rm -f "$T/root/covers/restart.stamp"
rc=$(OFFLINE= COVER_URL="$URL" run "$T" "$SHELFQUIT" "")
expect "[ $rc = 0 ]" "second start: exit code 0 (got $rc)"
expect "cmp -s '$T/root/covers/MegaDrive/Sonic The Hedgehog (USA, Europe).png' '$SRV/Sonic The Hedgehog (USA, Europe).png'" "second start: the cover was prefetched and saved"
expect "awk '/\[prefetch\] 1 of 1 fetched/{p=NR} /\[jailbreak\] pid/{j=NR} END{exit !(p && j && p<j)}' $T/root/logs/boot.log" "the download happened before the request for /data"
expect "! grep -q 'restarting' $T/root/logs/boot.log" "second start: no restart (nothing new)"
expect "[ ! -s $T/root/covers/wanted.txt ]" "second start: the wanted list is empty"
kill $SRVPID 2>/dev/null
stop_helpers
fi

if want 19; then
echo "== 19. the system folders: made in roms/ and covers/ at start; covers cached by older builds are moved"
T=$(newroot t19)
mkdir -p "$T/root/covers"
$ROM "$T/root/roms/Sonic The Hedgehog (USA, Europe).md" ntsc >/dev/null
python3 -c "from PIL import Image; Image.new('RGB',(512,357),(0,0,255)).save('$T/root/covers/md - Sonic The Hedgehog (USA, Europe).png')"
echo x >"$T/root/covers/gg - Some Game (USA).missing"
rc=$(run "$T" "0:0;60:$OPTIONS;62:0;70:$CROSS;72:0" "50")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
for d in MegaDrive SegaCD MasterSystem GameGear SG1000; do
	expect "[ -d $T/root/roms/$d ] && [ -d $T/root/covers/$d ]" "roms/$d and covers/$d made"
done
expect "[ -f '$T/root/covers/MegaDrive/Sonic The Hedgehog (USA, Europe).png' ] && [ ! -f '$T/root/covers/md - Sonic The Hedgehog (USA, Europe).png' ]" "an old 'md - ' cover moved to covers/MegaDrive"
expect "[ -f '$T/root/covers/GameGear/Some Game (USA).missing' ]" "an old .missing marker moved too"
expect "$CHECK $T/dump/flip00050.ppm 960 420 0 0 255 >/dev/null" "the moved cover is on the shelf"
expect "grep -q 'moved 2 cached cover file(s)' $T/root/logs/boot.log" "logged the move"
nosan "$T"
fi

echo
echo "passed $PASS, failed $FAIL  (work dir $WORK)"
[ $FAIL = 0 ]
