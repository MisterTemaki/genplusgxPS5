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
# the debug logs are off by default (2.0): the tests read boot.log, so here a settings file without the key means on
export GENPLUS_LOGS_DEFAULT=1
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
# the shelf: Options, then Cross on "Quit"
SHELFQUIT_AT() { echo "$1:$OPTIONS;$(($1 + 2)):0;$(($1 + 10)):$CROSS;$(($1 + 12)):0"; }
QUITAT() { echo "$1:$L3R3;$(($1 + 2)):0;$(($1 + 10)):$UP;$(($1 + 12)):0;$(($1 + 20)):$CROSS;$(($1 + 22)):0"; }

ok() { echo "  ok: $*"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $*"; FAIL=$((FAIL + 1)); }
expect() { if eval "$1"; then ok "$2"; else bad "$2"; fi; }
want() { [ -z "$ONLY" ] || [[ " $ONLY " == *" $1 "* ]]; }

newroot() {
	local t=$WORK/$1
	rm -rf "$t" && mkdir -p "$t/root/roms" "$t/dump"
	echo "shader=0" >"$t/root/genplus-ps5.ini" # the plain picture: the colour checks expect it (group 21 tests the shaders)
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
expect "[ -f $T/root/states/MegaDrive/test.state1 ]" "L2 + Up wrote states/MegaDrive/test.state1"
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
printf 'scale=1\nscanlines=1\n' >>"$T/root/genplus-ps5.ini"
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
lat=$(grep -o 'audio queued [0-9]* ([0-9]* ms' "$T/root/logs/boot.log" | sed -n 3p | grep -o '([0-9]*' | tr -d '(')
under=$(( ${u3:-999} - ${u2:-0} ))
expect "[ $under -le 2 ]" "audio underruns over 5 s of play: $under"
# the floor is low: a slow test machine (ASan, 2 cores) runs a little under 60 fps and drains the ring toward it
expect "[ ${lat:-0} -ge 15 ] && [ ${lat:-0} -le 120 ]" "audio latency ${lat:-?} ms"
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
APP=$T/homebrew/PPSA99921
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
META=$T/appmeta/PPSA99921
mkdir -p "$META" && echo old > "$META/pic0.png" && echo old > "$META/icon0.png" && cp app/sce_sys/param.json "$META/"
echo old > "$APP/sce_sys/pic0.png"
inst "$T"
expect "cmp -s $META/pic0.dds app/sce_sys/pic0.dds && cmp -s $META/pic1.dds app/sce_sys/pic1.dds && cmp -s $META/icon0.png app/sce_sys/icon0.png" "appmeta gets the background (pic0/pic1.dds) and icon"
expect "[ ! -f $META/pic0.png ] && [ ! -f $APP/sce_sys/pic0.png ]" "an old pic0.png is removed"
expect "grep -q 'Home screen art updated' $T/inst.txt" "the notification says the home screen art changed"
stop_helpers
expect "[ \$(find $T -path '*PPSA9992[234]*' -o -path '*PPSA9900[0-9]*' -o -path '*PPSA9901[0-9]*' -o -path '*PPSA99203*' | wc -l) = 0 ]" "nothing written for the other ports (PPSA99922-24, their old PPSA99009-12) or PS5SX2 (PPSA99203)"
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
# Triangle -> settings ("Shader" first, Down -> "Screen size"), Right -> integer scale, Circle -> back; Cross -> play
rc=$(GENPLUS_HOST_REALTIME=1 run "$T" "0:0;30:$TRIANGLE;32:0;36:$DOWN;38:0;40:$RIGHT;42:0;50:$CIRCLE;52:0;70:$CROSS;72:0;150:$R2;250:0;300:$L2R2;340:0;360:$L3R3;362:0;370:$DOWN;372:0;380:$CROSS;382:0;400:$CIRCLE;402:0;$(QUITAT 440)" "45")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'scale=1' $T/root/genplus-ps5.ini" "the settings screen changed the screen size"
expect "grep -q 'fast forward on' $T/root/logs/boot.log && grep -q 'fast forward off' $T/root/logs/boot.log" "R2 held: fast forward"
expect "grep -q 'rewind on' $T/root/logs/boot.log && grep -q 'rewind off' $T/root/logs/boot.log" "L2 + R2 held: rewind"
expect "[ -f '$T/root/states/MegaDrive/Alpha (USA).state1' ]" "the pause menu's Save state wrote slot 1"
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
echo "== 18. covers in the background: the helper downloads them while the shelf runs; an older helper: the prefetch"
waitfor() { for i in $(seq 1 50); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
stop_helpers() { pkill -f 'build/host/genplus-ps5-(installer|helper)' 2>/dev/null; pkill -f 'received.elf' 2>/dev/null; sleep 0.3; }
stop_helpers
T=$(newroot t18)
SRV=$T/srv/Sega_-_Mega_Drive_-_Genesis/Named_Boxarts; mkdir -p "$SRV" "$T/root/covers"
COVER="Sonic The Hedgehog (USA, Europe).png"
python3 -c "from PIL import Image; Image.new('RGB',(512,357),(255,0,0)).save('$SRV/$COVER')"
$ROM "$T/root/roms/Sonic The Hedgehog (USA, Europe).md" ntsc >/dev/null
PORT=18081
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
GENPLUS_PS5_ROOT=$T/root ASAN_OPTIONS=detect_leaks=0 timeout 120 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/root/logs/helper.log" "listening" || sleep 1
URL="http://127.0.0.1:$PORT/\${repo}/Named_Boxarts/\${name}.png"
rc=$(OFFLINE= COVER_URL="$URL" GENPLUS_HOST_REALTIME=1 run "$T" "0:0;300:$OPTIONS;302:0;310:$CROSS;312:0" "20,280")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'the helper downloads the covers while the app runs: nothing to wait for' $T/root/logs/boot.log && ! grep -q 'restarting' $T/root/logs/boot*.log" "the app starts at once: no download before /data, no restart"
expect "grep -q 'MegaDrive/Sonic The Hedgehog (USA, Europe).png' $T/root/covers/wanted.txt" "the missing cover goes to covers/wanted.txt"
expect "grep -q 'MegaDrive/Sonic The Hedgehog (USA, Europe).png -> 200' $T/root/logs/helper.log && cmp -s '$T/root/covers/MegaDrive/$COVER' '$SRV/$COVER'" "the helper downloaded it into covers/MegaDrive"
expect "! $CHECK $T/dump/flip00020.ppm 960 420 255 0 0 >/dev/null 2>&1 && $CHECK $T/dump/flip00280.ppm 960 420 255 0 0 >/dev/null" "the shelf showed its card, then the cover once it landed"
expect "grep -q '^0 1 idle' $T/root/covers/progress.txt" "covers/progress.txt: nothing left, 1 fetched"
kill $SRVPID 2>/dev/null
stop_helpers
# an older helper (1.4's protocol: the list without "covers: background"): the prefetch before /data, as before
T=$(newroot t18b)
mkdir -p "$T/srv/Sega_-_Mega_Drive_-_Genesis/Named_Boxarts" "$T/root/covers"
cp "$SRV/$COVER" "$T/srv/Sega_-_Mega_Drive_-_Genesis/Named_Boxarts/"
$ROM "$T/root/roms/Sonic The Hedgehog (USA, Europe).md" ntsc >/dev/null
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
printf 'MegaDrive/%s\thttp://127.0.0.1:%s/Sega_-_Mega_Drive_-_Genesis/Named_Boxarts/Sonic%%20The%%20Hedgehog%%20%%28USA%%2C%%20Europe%%29.png\n' "$COVER" $PORT >"$T/wanted.txt"
python3 - "$GENPLUS_HELPER_PORT" "$T/wanted.txt" <<'PY' &
import socket, struct, sys, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', int(sys.argv[1]))); s.listen(4); s.settimeout(60)
text = open(sys.argv[2], 'rb').read()
end = time.time() + 60
while time.time() < end:
    try:
        c, _ = s.accept()
    except OSError:
        break
    req = b''
    while len(req) < 0xA10:
        d = c.recv(0xA10 - len(req))
        if not d: break
        req += d
    if len(req) == 0xA10:
        magic, cmd, pid, ret = struct.unpack_from('<IiiI', req, 0)
        out = bytearray(req)
        if cmd == 6:
            struct.pack_into('<i', out, 12, len(text)); c.sendall(bytes(out) + text)
        elif cmd == 5:
            struct.pack_into('<i', out, 12, 0); out[16:18] = b'ok'; c.sendall(bytes(out))
    c.close()
PY
OLDPID=$!
sleep 0.5
rc=$(OFFLINE= COVER_URL="$URL" run "$T" "0:0;30:$OPTIONS;32:0;40:$CROSS;42:0" "")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '\[prefetch\] 1 of 1 fetched' $T/root/logs/boot.log && cmp -s '$T/root/covers/MegaDrive/$COVER' '$SRV/$COVER'" "an older helper: the cover is prefetched before /data and saved"
kill $OLDPID $SRVPID 2>/dev/null
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

if want 20; then
echo "== 20. controls: pad type and 4-player adapter, the button layout (Settings, CONTROLS)"
T=$(newroot t20)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
# B moved from Cross to Triangle: Cross does nothing, Triangle is B (green)
printf 'btn_b=3\npad_type=2\n' >>"$T/root/genplus-ps5.ini"
rc=$(run "$T" "0:0;100:$CROSS;140:0;160:$TRIANGLE;200:0;$(QUITAT 220)" "130,190" "$T/root/roms/test.md")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "$CHECK $T/dump/flip00130.ppm 960 540 red >/dev/null" "B on Triangle: Cross no longer presses B"
expect "$CHECK $T/dump/flip00190.ppm 960 540 green >/dev/null" "B on Triangle: Triangle presses B"
expect "grep -q 'controllers: 6 buttons pad' $T/root/logs/boot.log" "pad_type=2: a 6-button pad"
T=$(newroot t20b)
$ROM "$T/root/roms/test.sms" >/dev/null
printf 'pad_type=2\nmultitap=2\n' >>"$T/root/genplus-ps5.ini"
rc=$(GENPLUS_HOST_QUIT_AFTER=3 run "$T" "0:0" "" "$T/root/roms/test.sms")
expect "grep -q 'controllers: Auto (the game.s) pad$' $T/root/logs/boot.log" "a Master System game keeps its own pad"
T=$(newroot t20c)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
printf 'multitap=2\n' >>"$T/root/genplus-ps5.ini"
rc=$(GENPLUS_HOST_QUIT_AFTER=3 run "$T" "0:0" "" "$T/root/roms/test.md")
expect "grep -q 'controllers: Auto (the game.s) pad, Team Player (Sega)' $T/root/logs/boot.log" "Team Player plugged in"
T=$(newroot t20d)
$ROM "$T/root/roms/Alpha (USA).md" ntsc >/dev/null
# (1.8: the "CD add-on" row after "Sprite limit": one more Down; 2.1: the "Border" row after "Aspect ratio": one more, at 35)
rc=$(run "$T" "0:0;30:$TRIANGLE;32:0;35:$DOWN;37:0;40:$DOWN;42:0;46:$DOWN;48:0;52:$DOWN;54:0;58:$DOWN;60:0;64:$DOWN;66:0;70:$DOWN;72:0;76:$DOWN;78:0;82:$DOWN;84:0;88:$DOWN;90:0;94:$DOWN;96:0;100:$DOWN;102:0;106:$DOWN;108:0;112:$DOWN;114:0;118:$DOWN;120:0;124:$DOWN;126:0;130:$DOWN;132:0;136:$DOWN;138:0;142:$DOWN;144:0;148:$RIGHT;150:0;154:$RIGHT;156:0;160:$DOWN;162:0;166:$RIGHT;168:0;172:$DOWN;174:0;178:$RIGHT;180:0;188:$CIRCLE;190:0;198:$OPTIONS;200:0;206:$CROSS;208:0" "184")
expect "[ $rc = 0 ]" "settings screen: exit code 0 (got $rc)"
expect "grep -q '^pad_type=2$' $T/root/genplus-ps5.ini && grep -q '^multitap=1$' $T/root/genplus-ps5.ini" "the settings screen set a 6-button pad and the 4 Way Play"
expect "grep -q '^btn_a=3$' $T/root/genplus-ps5.ini" "the settings screen moved A to Triangle"
nosan "$T"
fi

if want 21; then
echo "== 21. CRT shaders: CRT Easymode style by default, every shader draws, the choice is kept"
SHADERS=("Off" "CRT Easymode style" "crt-lottes" "crt-lottes-fast" "crt-1tap" "crt-2tap" "crt-hyllian-fast" "crt-nobody" "newpixie-mini" "crt-blurPi-sharp" "crt-blurPi-soft" "monoCRT")
T=$(newroot t21)
rm -f "$T/root/genplus-ps5.ini" # a first start: no settings file yet
$ROM "$T/root/roms/test.md" ntsc >/dev/null
rc=$(run "$T" "0:0;100:$CROSS;140:0;$(QUITAT 160)" "90,130" "$T/root/roms/test.md")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'shader CRT Easymode style' $T/root/logs/boot.log" "a first start draws the game through CRT Easymode style"
expect "python3 - $T/dump/flip00090.ppm <<'PY'
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3)
w, h = map(int, d[1].split()); px = d[3]
R = lambda x, y: px[(y * w + x) * 3]
col = [R(960, y) for y in range(400, 460)] # down the middle: scanlines
row = [R(x, 540) for x in range(900, 960)] # across: the aperture grille
sys.exit(0 if max(col) > 150 and min(col) < 0.85 * max(col) and len(set(row)) > 1 else 1)
PY" "scanlines and a phosphor mask on the red picture"
expect "$CHECK $T/dump/flip00130.ppm 960 540 green >/dev/null || python3 -c \"import sys; d=open('$T/dump/flip00130.ppm','rb').read().split(b'\\n',3); w=int(d[1].split()[0]); p=d[3]; sys.exit(0 if max(p[(y*w+960)*3+1] for y in range(520,560)) > 150 else 1)\"" "Cross -> green through the shader"
expect "grep -q 'shader [0-9.]* ms' $T/root/logs/boot.log" "the shader's drawing time is logged"
nosan "$T"
for s in $(seq 1 11); do
	T=$(newroot t21s$s)
	echo "shader=$s" >>"$T/root/genplus-ps5.ini"
	$ROM "$T/root/roms/test.md" ntsc >/dev/null
	rc=$(run "$T" "0:0;$(QUITAT 100)" "90" "$T/root/roms/test.md")
	expect "[ $rc = 0 ] && grep -q 'shader ${SHADERS[$s]})' $T/root/logs/boot.log && python3 -c \"import sys; d=open('$T/dump/flip00090.ppm','rb').read().split(b'\\n',3); w=int(d[1].split()[0]); p=d[3]; sys.exit(0 if max(p[(y*w+960)*3] for y in range(500,580)) > 60 else 1)\"" "${SHADERS[$s]} draws the picture"
	expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "${SHADERS[$s]}: no sanitizer reports"
done
T=$(newroot t21m)
$ROM "$T/root/roms/Alpha (USA).md" ntsc >/dev/null
# Triangle -> settings, the first row is Shader: Right twice -> crt-lottes-fast; Circle -> back; Options + Cross -> quit
rc=$(run "$T" "0:0;30:$TRIANGLE;32:0;40:$RIGHT;42:0;46:$RIGHT;48:0;52:$RIGHT;54:0;60:$CIRCLE;62:0;70:$OPTIONS;72:0;80:$CROSS;82:0" "50")
expect "grep -q '^shader=3$' $T/root/genplus-ps5.ini" "the settings screen's Shader row is saved (shader=0 -> 3)"
fi

if want 22; then
echo "== 22. the helper: unknown titles refused, a slow client can't block it, wanted.txt must be a plain file"
waitfor() { for i in $(seq 1 50); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
stop_helpers() { pkill -f 'build/host/genplus-ps5-(installer|helper)' 2>/dev/null; pkill -f 'received.elf' 2>/dev/null; sleep 0.3; }
stop_helpers
T=$(newroot t22)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
# a process whose title can't be read (here: the host reports none) is not let out
GENPLUS_HOST_JB_TITLE= GENPLUS_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/hroot/logs/helper.log" "listening" || sleep 1
rc=$(run "$T" "$(QUITAT 30)" "" "$T/root/roms/test.md")
expect "grep -q 'title unknown: not Genesis Plus GX PS5' $T/hroot/logs/helper.log && ! grep -q 'letting it out' $T/hroot/logs/helper.log" "a process of unknown title is refused (fail closed)"
stop_helpers
# a client that sends one byte a second doesn't hold the helper: the app's request still goes through
T=$(newroot t22b)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
GENPLUS_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/hroot/logs/helper.log" "listening" || sleep 1
python3 - "$GENPLUS_HELPER_PORT" <<'PY' &
import socket, sys, time
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])))
try:
    for i in range(20):
        s.send(b'x'); time.sleep(1)
except OSError:
    pass
PY
DRIP=$!
sleep 0.5
start=$(date +%s)
rc=$(run "$T" "$(QUITAT 30)" "" "$T/root/roms/test.md")
secs=$(( $(date +%s) - start ))
expect "grep -q 'Genesis Plus GX helper (port [0-9]*): ret 0' $T/root/logs/boot.log" "with a slow client connected, the app is still let out"
expect "[ $secs -lt 15 ]" "and without waiting for the slow client (${secs}s)"
kill $DRIP 2>/dev/null
stop_helpers
# covers/wanted.txt as a symbolic link to another file: not read
T=$(newroot t22c)
mkdir -p "$T/hroot/covers"
echo "secret" >"$T/secret.txt"
ln -s "$T/secret.txt" "$T/hroot/covers/wanted.txt"
GENPLUS_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/hroot/logs/helper.log" "listening" || sleep 1
python3 - "$GENPLUS_HELPER_PORT" "$T/answer.bin" <<'PY'
import socket, struct, sys
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])))
req = bytearray(0xA10)
struct.pack_into('<IiI', req, 0, 0xDEADBEEF, 6, 1234)
s.sendall(req)
data = b''
while True:
    d = s.recv(65536)
    if not d: break
    data += d
open(sys.argv[2], 'wb').write(data)
PY
expect "grep -q 'wanted-covers list, 0 bytes' $T/hroot/logs/helper.log && ! grep -q secret $T/answer.bin" "a wanted.txt that is a symbolic link is not followed"
stop_helpers
fi

if want 23; then
echo "== 23. saves: one folder per system, old saves moved, an erased battery save is written, unchanged ones aren't"
# a Mega Drive and a Master System game of the same name: each its own state
T=$(newroot t23)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
$ROM "$T/root/roms/test.sms" >/dev/null
rc=$(run "$T" "0:0;100:$L2UP;105:0;$(QUITAT 140)" "" "$T/root/roms/test.md")
rc2=$(run "$T" "0:0;100:$L2DOWN;105:0;$(QUITAT 140)" "" "$T/root/roms/test.sms")
expect "[ $rc = 0 ] && [ $rc2 = 0 ]" "exit codes 0 (got $rc, $rc2)"
expect "[ -f $T/root/states/MegaDrive/test.state1 ] && [ ! -e $T/root/states/MasterSystem/test.state1 ]" "the Mega Drive state went to states/MegaDrive/"
expect "! grep -q 'load state 1' $T/root/logs/boot.log" "the Master System game of the same name didn't load it"
nosan "$T"
# a state of the first builds (directly in states/) is moved to the system's folder and loads
T=$(newroot t23b)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
rc=$(run "$T" "0:0;100:$L2UP;105:0;$(QUITAT 140)" "" "$T/root/roms/test.md")
mv "$T/root/states/MegaDrive/test.state1" "$T/root/states/test.state1"
rc=$(run "$T" "0:0;100:$L2DOWN;105:0;$(QUITAT 140)" "" "$T/root/roms/test.md")
expect "[ ! -e $T/root/states/test.state1 ] && [ -f $T/root/states/MegaDrive/test.state1 ]" "an old flat state was moved to states/MegaDrive/"
expect "grep -q 'load state 1: ok' $T/root/logs/boot.log" "and loads"
nosan "$T"
# battery RAM: the game writes \$42; then erases it (\$FF while B is held) -- the erase must reach the file
T=$(newroot t23c)
$ROM "$T/root/roms/save.md" sram >/dev/null
rc=$(run "$T" "0:0;$(QUITAT 100)" "" "$T/root/roms/save.md")
S=$T/root/saves/MegaDrive/save.srm
expect "[ -f $S ] && python3 -c 'import sys; sys.exit(0 if 0x42 in open(\"$S\",\"rb\").read() else 1)'" "the battery save holds what the game wrote"
ino=$(stat -c %i "$S" 2>/dev/null)
rc=$(run "$T" "0:0;$(QUITAT 100)" "" "$T/root/roms/save.md")
expect "[ \"$(stat -c %i "$S" 2>/dev/null)\" = '$ino' ]" "an unchanged battery save isn't written again"
rc=$(run "$T" "0:0;20:$CROSS;$(QUITAT 100)" "" "$T/root/roms/save.md")
expect "[ -f $S ] && python3 -c 'import sys; sys.exit(1 if 0x42 in open(\"$S\",\"rb\").read() else 0)'" "the game's erase was saved"
nosan "$T"
fi

if want 24; then
echo "== 24. audit fixes: 720p picture edges, Resume doesn't press B, odd library files, long paths, covers kept, downloads after a tab change"
SQUARE=8000
# 720p: the whole picture follows the game, its left edge included (the damage is mapped to 1280x720)
T=$(newroot t24)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
rc=$(GENPLUS_HOST_DIRECT_MAX_MIB=12 run "$T" "0:0;100:$CROSS;140:0;$(QUITAT 200)" "130" "$T/root/roms/test.md")
expect "grep -q 'scan-out 1280x720' $T/root/logs/boot.log" "720p scan-out"
expect "$CHECK $T/dump/flip00130.ppm 180 360 green >/dev/null && $CHECK $T/dump/flip00130.ppm 1100 360 green >/dev/null" "720p: Cross turns the picture green to its edges"
nosan "$T"
# Resume with Cross: while Cross is still held the game doesn't see B (the picture stays red)
T=$(newroot t24b)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
rc=$(run "$T" "0:0;100:$L3R3;102:0;120:$CROSS;200:0;$(QUITAT 260)" "190" "$T/root/roms/test.md")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "$CHECK $T/dump/flip00190.ppm 960 540 red >/dev/null" "Cross held after Resume doesn't reach the game"
nosan "$T"
# the library: a FIFO doesn't hang the scan; an unquoted cue FILE hides its track; a zip entry with a 600-character name
T=$(newroot t24c)
mkdir -p "$T/root/roms/MegaDrive" "$T/root/roms/SegaCD/Disc"
mkfifo "$T/root/roms/MegaDrive/pipe.md"
$ROM "$T/root/roms/MegaDrive/a.md" ntsc >/dev/null
$ROM "$T/root/roms/MegaDrive/b.md" ntsc >/dev/null
printf 'FILE track01.bin BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n' >"$T/root/roms/SegaCD/Disc/Disc.cue"
head -c 4096 /dev/zero >"$T/root/roms/SegaCD/Disc/track01.bin"
python3 - "$T/root/roms/MegaDrive/long.zip" "$T/root/roms/MegaDrive/a.md" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w') as z:
    z.writestr('x' * 600 + '.md', b'\0' * 1024)
    z.write(sys.argv[2], 'inner.md')
PY
rc=$(MAXFLIPS=400 run "$T" "0:0;30:$OPTIONS;32:0;40:$CROSS;42:0" "")
expect "[ $rc = 0 ]" "the shelf came up and quit (got $rc)"
expect "! grep -q 'pipe.md' $T/root/logs/boot.log" "a FIFO is not listed (nor read)"
expect "! grep -q 'track01.bin \[' $T/root/logs/boot.log && grep -q 'Disc.cue \[' $T/root/logs/boot.log" "FILE track01.bin (no quotes): the track is the disc's, not a game"
expect "grep -q 'long.zip \[md\]' $T/root/logs/boot.log" "a zip with a 600-character entry name still lists its game"
expect "grep -q 'MegaDrive/b.md' $T/root/covers/crc-cache.txt" "the CRC cache holds b.md"
rm -f "$T/root/roms/MegaDrive/b.md"
rc=$(MAXFLIPS=400 run "$T" "0:0;30:$OPTIONS;32:0;40:$CROSS;42:0" "")
expect "! grep -q 'MegaDrive/b.md' $T/root/covers/crc-cache.txt" "a deleted ROM leaves the CRC cache"
nosan "$T"
# a path of 255 characters or more: refused with a message, nothing overflows
T=$(newroot t24d)
D="$T/root/roms/$(printf 'd%.0s' $(seq 1 120))/$(printf 'e%.0s' $(seq 1 120))"
mkdir -p "$D"
$ROM "$D/test.md" ntsc >/dev/null
rc=$(MAXFLIPS=300 run "$T" "0:0;100:$CROSS;102:0" "" "$D/test.md")
expect "grep -q 'the path is [0-9]* characters long' $T/root/logs/boot.log" "a too-long path is refused"
nosan "$T"
# Square (fetch the cover again) while offline: the cover it has is kept
T=$(newroot t24e)
mkdir -p "$T/root/roms/MegaDrive" "$T/root/covers/MegaDrive"
$ROM "$T/root/roms/MegaDrive/Sonic The Hedgehog (USA, Europe).md" ntsc >/dev/null
python3 -c "from PIL import Image; Image.new('RGB',(512,357),(255,0,0)).save('$T/root/covers/MegaDrive/Sonic The Hedgehog (USA, Europe).png')"
rc=$(run "$T" "0:0;40:$SQUARE;42:0;80:$OPTIONS;82:0;90:$CROSS;92:0" "")
expect "[ -s '$T/root/covers/MegaDrive/Sonic The Hedgehog (USA, Europe).png' ]" "offline, Square doesn't delete the cover"
nosan "$T"
# online, Square on a ROM named as its official name (its downloaded cover is also "the cover named after the ROM"):
# the server has none (404) -> the cover stays; the server has it -> the new one replaces it
cp "$T/root/covers/MegaDrive/Sonic The Hedgehog (USA, Europe).png" "$T/old.png"
SRV=$T/srv/Sega_-_Mega_Drive_-_Genesis/Named_Boxarts; mkdir -p "$SRV"
PORT=18094
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
sleep 1
URL="http://127.0.0.1:$PORT/\${repo}/Named_Boxarts/\${name}.png"
SQ="0:0;40:$SQUARE;42:0;200:$OPTIONS;202:0;210:$CROSS;212:0"
rc=$(OFFLINE= COVER_URL="$URL" GENPLUS_HOST_REALTIME=1 run "$T" "$SQ" "")
expect "cmp -s '$T/root/covers/MegaDrive/Sonic The Hedgehog (USA, Europe).png' $T/old.png && [ ! -e '$T/root/covers/MegaDrive/Sonic The Hedgehog (USA, Europe).refetch' ]" "the server has none (404): the cover stays"
python3 -c "from PIL import Image; Image.new('RGB',(512,357),(0,255,0)).save('$SRV/Sonic The Hedgehog (USA, Europe).png')"
rm -f "$T/root/covers/MegaDrive/"*.missing
rc=$(OFFLINE= COVER_URL="$URL" GENPLUS_HOST_REALTIME=1 run "$T" "$SQ" "")
kill $SRVPID 2>/dev/null
expect "cmp -s '$T/root/covers/MegaDrive/Sonic The Hedgehog (USA, Europe).png' '$SRV/Sonic The Hedgehog (USA, Europe).png'" "the server has it: the new cover replaces the old one"
nosan "$T"
# as on the console (a helper answers, so the shelf downloads nothing): Square puts the cover on the wanted list
# and the helper fetches it in the background (1.5; no restart); the old cover stays until the new one lands
T2=$(newroot t24g)
mkdir -p "$T2/root/roms/MegaDrive" "$T2/root/covers/MegaDrive"
$ROM "$T2/root/roms/MegaDrive/Sonic The Hedgehog (USA, Europe).md" ntsc >/dev/null
cp "$T/old.png" "$T2/root/covers/MegaDrive/Sonic The Hedgehog (USA, Europe).png"
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
GENPLUS_PS5_ROOT=$T2/root ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T2/helper.txt" 2>&1 &
for i in $(seq 1 50); do grep -q listening "$T2/root/logs/helper.log" 2>/dev/null && break; sleep 0.1; done
rc=$(OFFLINE= COVER_URL="$URL" GENPLUS_HOST_REALTIME=1 run "$T2" "0:0;40:$SQUARE;42:0;300:$OPTIONS;302:0;310:$CROSS;312:0" "")
expect "grep -q 'MegaDrive/Sonic The Hedgehog (USA, Europe).png' $T2/root/covers/wanted.txt && ! grep -q 'restarting' $T2/root/logs/boot*.log" "console: Square lists the cover for the helper, no restart"
expect "grep -q 'MegaDrive/Sonic The Hedgehog (USA, Europe).png -> 200' $T2/root/logs/helper.log && cmp -s '$T2/root/covers/MegaDrive/Sonic The Hedgehog (USA, Europe).png' '$SRV/Sonic The Hedgehog (USA, Europe).png' && [ ! -e '$T2/root/covers/MegaDrive/Sonic The Hedgehog (USA, Europe).refetch' ]" "the helper fetched the new cover over the old one"
kill $SRVPID 2>/dev/null
pkill -f "$HELPER" 2>/dev/null; sleep 0.3
# downloads still work after the shelf restarted its cover worker (another tab)
T=$(newroot t24f)
SRV=$T/srv/Sega_-_Mega_Drive_-_Genesis/Named_Boxarts; mkdir -p "$SRV" "$T/root/roms/MegaDrive" "$T/root/roms/MasterSystem" "$T/root/covers/MasterSystem"
python3 -c "from PIL import Image; Image.new('RGB',(512,357),(255,0,0)).save('$SRV/Sonic The Hedgehog (USA, Europe).png'); Image.new('RGB',(360,512),(0,0,255)).save('$T/root/covers/MasterSystem/Alex Kidd in Miracle World (USA, Europe).png')"
$ROM "$T/root/roms/MegaDrive/Sonic The Hedgehog (USA, Europe).md" ntsc >/dev/null
$ROM "$T/root/roms/MasterSystem/Alex Kidd in Miracle World (USA, Europe).sms" >/dev/null
echo "shelf_family=3" >>"$T/root/genplus-ps5.ini"
PORT=18082
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
sleep 1
rc=$(OFFLINE= COVER_URL="http://127.0.0.1:$PORT/\${repo}/Named_Boxarts/\${name}.png" GENPLUS_HOST_REALTIME=1 run "$T" \
	"0:0;60:$DOWN;62:0;300:$OPTIONS;302:0;310:$CROSS;312:0" "")
kill $SRVPID 2>/dev/null
expect "grep -q 'tab All games' $T/root/logs/boot.log" "moved to another tab (the cover worker restarted)"
expect "cmp -s '$T/root/covers/MegaDrive/Sonic The Hedgehog (USA, Europe).png' '$SRV/Sonic The Hedgehog (USA, Europe).png'" "the new tab's cover was downloaded"
nosan "$T"
fi

if want 25; then
echo "== 25. debug logs off: nothing written (app and helper), earlier logs kept; the setting switches them at once"
# off in the ini: the game runs, no line anywhere, the previous run's boot.log untouched
T=$(newroot t25)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
echo "debug_logs=0" >>"$T/root/genplus-ps5.ini"
mkdir -p "$T/root/logs" && echo "OLD RUN" >"$T/root/logs/boot.log"
rc=$(run "$T" "0:0;$(QUITAT 100)" "90" "$T/root/roms/test.md")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "$CHECK $T/dump/flip00090.ppm 960 540 red >/dev/null" "the game runs"
expect "[ \"\$(cat $T/root/logs/boot.log)\" = 'OLD RUN' ] && [ ! -e $T/root/logs/boot.prev.log ]" "the earlier boot.log is kept as it was, nothing new written"
expect "! grep -q '^\[genplus-ps5' $T/out.txt" "nothing on stdout either"
expect "grep -q '^debug_logs=0' $T/root/genplus-ps5.ini" "the setting stays off"
nosan "$T"
# the default (2.0): no debug_logs key and no test override -> nothing written, and the settings file says 0
T=$(newroot t25z)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
rc=$(GENPLUS_LOGS_DEFAULT= run "$T" "0:0;$(QUITAT 100)" "90" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && [ ! -e $T/root/logs/boot.log ] && ! grep -q '^\[genplus-ps5' $T/out.txt" "debug logs off by default: no boot.log"
expect "grep -q '^debug_logs=0' $T/root/genplus-ps5.ini && grep -q '^perf_log=0' $T/root/genplus-ps5.ini" "the default settings file has debug_logs=0 and perf_log=0"
nosan "$T"
# Settings (Triangle) -> Debug logs (Up three times from Shader: Back, Updates, then Debug logs) -> Off: that line is the last one
T=$(newroot t25b)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
rc=$(run "$T" "0:0;30:$TRIANGLE;32:0;50:$UP;52:0;60:$UP;62:0;65:$UP;67:0;70:$CROSS;72:0;90:$CIRCLE;92:0;$(SHELFQUIT_AT 120)" "")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '^debug_logs=0' $T/root/genplus-ps5.ini" "Debug logs Off saved"
expect "tail -1 $T/root/logs/boot.log | grep -q 'debug logs turned off'" "the last line of boot.log says the logs were turned off"
nosan "$T"
# and back on: logging starts again in the same boot.log
T=$(newroot t25c)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
echo "debug_logs=0" >>"$T/root/genplus-ps5.ini"
rc=$(run "$T" "0:0;30:$TRIANGLE;32:0;50:$UP;52:0;60:$UP;62:0;65:$UP;67:0;70:$CROSS;72:0;90:$CIRCLE;92:0;$(SHELFQUIT_AT 120)" "")
expect "grep -q '^debug_logs=1' $T/root/genplus-ps5.ini" "Debug logs On saved"
expect "grep -q 'debug logs turned on' $T/root/logs/boot.log && ! grep -q 'Genesis Plus GX PS5 1\.[0-9], Genesis' $T/root/logs/boot.log" "boot.log starts at the switch (nothing from before it)"
nosan "$T"
# the helper follows the setting too
stop_helpers25() { pkill -f 'build/host/genplus-ps5-(installer|helper)' 2>/dev/null; sleep 0.3; }
stop_helpers25
T=$(newroot t25d)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
mkdir -p "$T/hroot" && echo "debug_logs=0" >"$T/hroot/genplus-ps5.ini"
GENPLUS_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
sleep 1.5
rc=$(run "$T" "$(QUITAT 30)" "" "$T/root/roms/test.md")
expect "grep -q 'Genesis Plus GX helper (port [0-9]*): ret 0' $T/root/logs/boot.log" "the helper still lets the app out"
expect "[ ! -e $T/hroot/logs/helper.log ] && ! grep -q 'helper\]' $T/helper.txt" "with debug logs off the helper writes no log"
stop_helpers25
fi

if want 26; then
echo "== 26. the helper's own HTTPS (Mbed TLS): certificate checked, kept connection, chunks, redirects, 404"
stop_helpers26() { pkill -f 'build/host/genplus-ps5-(installer|helper)' 2>/dev/null; sleep 0.3; }
stop_helpers26
C=$WORK/t26-certs; rm -rf "$C"; mkdir -p "$C"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$C/ca.key" -out "$C/ca.pem" -days 30 -subj "/CN=Genplus Test CA" 2>/dev/null
openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$C/srv.key" -out "$C/srv.csr" -subj "/CN=localhost" 2>/dev/null
printf 'subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\n' >"$C/ext.cnf"
openssl x509 -req -in "$C/srv.csr" -CA "$C/ca.pem" -CAkey "$C/ca.key" -CAcreateserial -out "$C/srv.pem" -days 30 -extfile "$C/ext.cnf" 2>/dev/null
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$C/other.key" -out "$C/other.pem" -days 30 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost" 2>/dev/null
SRVROOT=$WORK/t26-srv; rm -rf "$SRVROOT"; mkdir -p "$SRVROOT/Repo/Named_Boxarts"
python3 -c "
from PIL import Image
for n,c in [('A b',(255,0,0)),('B',(0,255,0)),('C',(0,0,255))]: Image.new('RGB',(512,357),c).save('$SRVROOT/Repo/Named_Boxarts/'+n+'.png')"
PORT=18443
tls_case() { # name cert key mode host -> $T with the helper's run in it
	T=$(newroot "t26-$1")
	mkdir -p "$T/root/covers"
	python3 tests/https_server.py "$SRVROOT" $PORT "$2" "$3" "$4" 2>"$T/server.txt" &
	local sp=$!
	sleep 0.6
	local u="https://$5:$PORT"
	printf 'MegaDrive/A.png\t%s/Repo/Named_Boxarts/A%%20b.png\nMegaDrive/B.png\t%s/chunked/Repo/Named_Boxarts/B.png\nMegaDrive/C.png\t%s/moved/Repo/Named_Boxarts/C.png\nMegaDrive/D.png\t%s/Repo/Named_Boxarts/D.png\n' "$u" "$u" "$u" "$u" >"$T/root/covers/wanted.txt"
	GENPLUS_EXTRA_CA=$C/ca.pem GENPLUS_PS5_ROOT=$T/root ASAN_OPTIONS=detect_leaks=0 timeout ${6:-6} "$HELPER" >"$T/helper.txt" 2>&1
	kill $sp 2>/dev/null; wait $sp 2>/dev/null
	stop_helpers26
}
tls_case good "$C/srv.pem" "$C/srv.key" keep localhost
H="$T/root/logs/helper.log"; D="$T/root/covers/MegaDrive"; S="$SRVROOT/Repo/Named_Boxarts"
expect "grep -q '\[https\] Mbed TLS 3\.6\.[0-9]*: [0-9]* CA certificate(s), + the extra CA' $H" "Mbed TLS set up with Mozilla's CA list (+ the test CA)"
expect "cmp -s '$D/A.png' '$S/A b.png'" "a cover over HTTPS (Content-Length)"
expect "cmp -s '$D/B.png' '$S/B.png'" "a chunked answer"
expect "cmp -s '$D/C.png' '$S/C.png' && grep -q 'GET /moved/' $T/server.txt" "a redirect followed"
expect "[ -f '$D/D.missing' ] && [ ! -f '$D/D.png' ]" "a 404: marked missing"
expect "[ \$(grep -c 'kept connection' $H) = 3 ]" "one connection for the four requests"
expect "! grep -q '\[http\] ' $H" "the helper doesn't use libSceHttp2"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/helper.txt" "no sanitizer reports"
tls_case untrusted "$C/other.pem" "$C/other.key" keep localhost 4
H="$T/root/logs/helper.log"; D="$T/root/covers/MegaDrive"
expect "grep -q 'Certificate verification failed' $H && [ -z \"\$(ls $D 2>/dev/null)\" ]" "a certificate from an unknown CA: refused, nothing saved, nothing marked missing"
tls_case wrongname "$C/srv.pem" "$C/srv.key" keep 127.0.0.1 4
H="$T/root/logs/helper.log"; D="$T/root/covers/MegaDrive"
expect "grep -q 'Certificate verification failed' $H && [ -z \"\$(ls $D 2>/dev/null)\" ]" "a certificate for another name: refused"
tls_case closing "$C/srv.pem" "$C/srv.key" drop localhost
D="$T/root/covers/MegaDrive"
expect "[ -f '$D/A.png' ] && [ -f '$D/B.png' ] && [ -f '$D/C.png' ]" "a server that closes kept connections: a new one each time"
fi

if want 27; then
echo "== 27. the new title ID (PPSA99921): nothing under the old one (PPSA99011) is touched; our old version is named"
NEWID=PPSA99921; OLDID=PPSA99011
OURS="UP9000-${OLDID}_00-GENPLUSGXPS50001"; OTHER="UP9000-${OLDID}_00-ANOTHERAPP000001"
stop27() { pkill -f 'build/host/genplus-ps5-(installer|helper)' 2>/dev/null; sleep 0.3; }
waitfor27() { for i in $(seq 1 80); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
oldapp() { # dir content-id -> an app folder under the old ID: ours (as 1.6 left it) or another app's
	mkdir -p "$1/homebrew/$OLDID/sce_sys" "$1/homebrew/$OLDID/sce_module"
	sed "s/$NEWID/$OLDID/g; s/$OURS/$2/" app/sce_sys/param.json >"$1/homebrew/$OLDID/sce_sys/param.json"
	echo old-eboot >"$1/homebrew/$OLDID/eboot.bin"; echo old-icon >"$1/homebrew/$OLDID/sce_sys/icon0.png"
}
snap() { (cd "$1/homebrew/$OLDID" && find . -type f -exec md5sum {} + | sort); }
inst27() {
	GENPLUS_PS5_ROOT=$1/root GENPLUS_PS5_HOMEBREW=$1/homebrew GENPLUS_PS5_APPMETA=$1/appmeta ASAN_OPTIONS=detect_leaks=0 timeout 60 "$INSTALLER" >>"$1/inst.txt" 2>&1
}
stop27
# a: our old version under the old ID: left exactly as it is, the new one installed beside it, the notification names it
T=$(newroot t27a); oldapp "$T" "$OURS"; mkdir -p "$T/appmeta/$OLDID"; echo old >"$T/appmeta/$OLDID/icon0.png"
snap "$T" >"$T/before.txt"
inst27 "$T" &
expect "waitfor27 $T/root/logs/installer.log 'listening on 127.0.0.1'" "installed, then stays as the helper"
expect "cmp -s $T/homebrew/$NEWID/eboot.bin tests/fake-eboot.bin && grep -q '\"titleId\": \"$NEWID\"' $T/homebrew/$NEWID/sce_sys/param.json" "the app installed as $NEWID"
expect "snap $T | cmp -s - $T/before.txt && grep -q old $T/appmeta/$OLDID/icon0.png" "the old folder $OLDID and its home-screen art: untouched"
expect "grep -q 'Its title ID is now $NEWID. The old version ($OLDID) is left alone: to remove it, delete its folder /data/homebrew/$OLDID' $T/inst.txt" "the notification names the old version and how to delete it"
stop27
# b: another app's folder under the old ID: untouched, and not named
T=$(newroot t27b); oldapp "$T" "$OTHER"; snap "$T" >"$T/before.txt"
inst27 "$T" &
expect "waitfor27 $T/root/logs/installer.log 'listening on 127.0.0.1'" "installed"
expect "snap $T | cmp -s - $T/before.txt && grep -q 'another app.s folder, left as it is' $T/root/logs/installer.log" "another app's $OLDID folder: untouched"
expect "! grep -q 'title ID is now' $T/inst.txt && cmp -s $T/homebrew/$NEWID/eboot.bin tests/fake-eboot.bin" "nothing said about it; ours installed as $NEWID"
stop27
# c: nothing under the old ID: a plain install
T=$(newroot t27c)
inst27 "$T" &
expect "waitfor27 $T/root/logs/installer.log 'listening on 127.0.0.1'" "installed"
expect "[ ! -e $T/homebrew/$OLDID ] && ! grep -q 'title ID is now' $T/inst.txt" "no old version: nothing made or said under $OLDID"
expect "! grep -q 'runtime error\|AddressSanitizer' $WORK/t27a/inst.txt $WORK/t27b/inst.txt $T/inst.txt" "no sanitizer reports"
stop27
fi

if want 28; then
echo "== 28. updates from GitHub's releases (Settings, SYSTEM, Updates): the helper checks, downloads, checks every file, installs"
T=$(newroot t28)
UPD="python3 tests/update_client.py $GENPLUS_HELPER_PORT"
stop28() { pkill -f 'build/host/genplus-ps5-(installer|helper)' 2>/dev/null; pkill -f 'http.server 1[89][0-9][0-9][0-9]' 2>/dev/null; sleep 0.3; }
waitfor28() { for i in $(seq 1 80); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
stop28
SRV=$T/srv; mkdir -p "$SRV" "$T/appmeta/PPSA99921"; echo old >"$T/appmeta/PPSA99921/icon0.png"
SPORT=$((18000 + RANDOM % 1900))
(cd "$SRV" && exec python3 -m http.server $SPORT --bind 127.0.0.1 >/dev/null 2>&1) &
# the release: the installed app with a new eboot, a newer param.json, a new file; the old manifest lists a file that goes
release() { # tag contentVersion [bad]
	rm -rf "$T/rel" && mkdir -p "$T/rel" && cp -r "$T/homebrew/PPSA99921" "$T/rel/PPSA99921" && rm -f "$T/rel/PPSA99921/manifest.sha256" "$T/rel/PPSA99921/sce_sys/old.bin"
	echo "eboot $1" >"$T/rel/PPSA99921/eboot.bin"; echo "new art" >"$T/rel/PPSA99921/sce_sys/icon0.png"; echo extra >"$T/rel/PPSA99921/sce_sys/extra.bin"
	sed -i "s/\"contentVersion\": \"[0-9.]*\"/\"contentVersion\": \"$2\"/" "$T/rel/PPSA99921/sce_sys/param.json"
	python3 tools/make_release_zip.py "$T/rel/PPSA99921" PPSA99921 "$SRV/GenesisPlusGXPS5-$1-PPSA99921.zip" >/dev/null
	local sha size
	sha=$(sha256sum "$SRV/GenesisPlusGXPS5-$1-PPSA99921.zip" | cut -c1-64); size=$(stat -c %s "$SRV/GenesisPlusGXPS5-$1-PPSA99921.zip")
	[ -n "${3:-}" ] && sha=0000${sha:4}
	printf '{"tag_name":"%s","name":"Genesis Plus GX PS5 %s","assets":[{"name":"GenesisPlusGXPS5-%s.elf","size":1,"browser_download_url":"http://127.0.0.1:%s/x.elf"},{"name":"GenesisPlusGXPS5-%s-PPSA99921.zip","size":%s,"digest":"sha256:%s","browser_download_url":"http://127.0.0.1:%s/GenesisPlusGXPS5-%s-PPSA99921.zip"}]}' \
		"$1" "$1" "$1" $SPORT "$1" $size $sha $SPORT "$1" >"$SRV/latest.json"
}
# the app installed by the payload (1.7), which then stays as the helper; the updater asks the test server
GENPLUS_UPDATE_API=http://127.0.0.1:$SPORT/latest.json GENPLUS_PS5_ROOT=$T/root GENPLUS_PS5_HOMEBREW=$T/homebrew GENPLUS_PS5_APPMETA=$T/appmeta \
	ASAN_OPTIONS=detect_leaks=0 timeout 120 "$INSTALLER" >"$T/inst.txt" 2>&1 &
expect "waitfor28 $T/root/logs/installer.log 'listening on 127.0.0.1'" "the payload installed 1.7 and serves as the helper"
expect "[ \"\$($UPD status 1.7 | cut -d'|' -f1)\" = idle ]" "the updater answers: idle"
release v1.7 01.070.000
expect "$UPD check 1.7 --until uptodate,failed | grep -q '^uptodate|1.7|'" "the newest release is this version: up to date"
echo gone >"$T/homebrew/PPSA99921/sce_sys/old.bin"
printf '%s *sce_sys/old.bin\n' "$(sha256sum <"$T/homebrew/PPSA99921/sce_sys/old.bin" | cut -c1-64)" >"$T/homebrew/PPSA99921/manifest.sha256"
release v9.9 99.000.000 bad
expect "$UPD check 1.7 --until available,failed | grep -q '^available|9.9|'" "a newer release (9.9): available, with its size"
cp "$T/homebrew/PPSA99921/eboot.bin" "$T/eboot.before"
expect "$UPD install 1.7 --until done,failed | grep -q \"^failed|.*SHA-256 doesn't match\"" "a download whose SHA-256 isn't GitHub's: refused"
expect "cmp -s $T/homebrew/PPSA99921/eboot.bin $T/eboot.before && [ -z \"\$(find $T/homebrew -name '*.new')\" ]" "and nothing was changed"
release v9.9 99.000.000
expect "$UPD check 1.7 --until available,failed | grep -q '^available|9.9|'" "checked again"
expect "$UPD install 1.7 --until done,failed | grep -q '^done|9.9|'" "downloaded, every file checked, installed"
expect "grep -q 'eboot v9.9' $T/homebrew/PPSA99921/eboot.bin && grep -q '\"contentVersion\": \"99.000.000\"' $T/homebrew/PPSA99921/sce_sys/param.json" "the new eboot.bin and param.json in place"
expect "[ -f $T/homebrew/PPSA99921/sce_sys/extra.bin ] && [ ! -e $T/homebrew/PPSA99921/sce_sys/old.bin ] && grep -q 'sce_sys/extra.bin' $T/homebrew/PPSA99921/manifest.sha256" "a new file added, one the old manifest had removed, the manifest kept"
expect "grep -q 'new art' $T/appmeta/PPSA99921/icon0.png && [ -z \"\$(find $T/homebrew -name '*.new')\" ]" "the home screen's copy of the art refreshed; no .new files left"
expect "$UPD status 9.9 | grep -q '^done|'" "the result stays until the app is opened again"
stop28
(cd "$SRV" && exec python3 -m http.server $SPORT --bind 127.0.0.1 >/dev/null 2>&1) &
# the old payload sent again (an autoload): the newer version is left alone
GENPLUS_UPDATE_API=http://127.0.0.1:$SPORT/latest.json GENPLUS_PS5_ROOT=$T/root GENPLUS_PS5_HOMEBREW=$T/homebrew GENPLUS_PS5_APPMETA=$T/appmeta \
	ASAN_OPTIONS=detect_leaks=0 timeout 120 "$INSTALLER" >"$T/inst2.txt" 2>&1 &
expect "waitfor28 $T/inst2.txt 'a newer version is installed'" "an older payload sent again: says a newer version is installed"
expect "grep -q 'eboot v9.9' $T/homebrew/PPSA99921/eboot.bin && grep -q 'new art' $T/appmeta/PPSA99921/icon0.png" "and leaves the update (and its art) as they are"
expect "$UPD check 9.9 --until uptodate,available,failed | grep -q '^uptodate|9.9|'" "the app says which version it is: an older helper finds it up to date"
stop28
# the app: at start the daily check (the helper finds 9.9); Settings (Triangle), Up twice to "Updates" (it shows
# "Install 9.9"), Cross: the helper installs it
T2=$(newroot t28b)
(cd "$SRV" && exec python3 -m http.server $SPORT --bind 127.0.0.1 >/dev/null 2>&1) &
GENPLUS_UPDATE_API=http://127.0.0.1:$SPORT/latest.json GENPLUS_PS5_ROOT=$T2/root GENPLUS_PS5_HOMEBREW=$T2/homebrew GENPLUS_PS5_APPMETA=$T2/appmeta \
	ASAN_OPTIONS=detect_leaks=0 timeout 120 "$INSTALLER" >"$T2/inst.txt" 2>&1 &
waitfor28 $T2/root/logs/installer.log 'listening on 127.0.0.1'
$ROM "$T2/root/roms/test.md" ntsc >/dev/null
rc=$(GENPLUS_HOST_REALTIME=1 GENPLUS_PS5_APPMETA=$T2/appmeta run "$T2" "0:0;30:$TRIANGLE;32:0;40:$UP;42:0;46:$UP;48:0;56:$CROSS;58:0;150:$CIRCLE;152:0;$(QUITAT 170)" "54,140")
expect "grep -q 'daily check asked of the helper' $T2/root/logs/boot.log && [ -f $T2/root/update-check.txt ]" "at start: the daily check asked of the helper"
expect "grep -q 'update install' $T2/root/logs/installer.log && grep -q 'eboot v9.9' $T2/homebrew/PPSA99921/eboot.bin" "Cross on \"Install 9.9\": the helper installs it"
expect "grep -q 'is available: Settings, SYSTEM, Updates' $T2/inst.txt $T2/root/logs/installer.log" "the daily check found 9.9: a notification says so"
nosan "$T2"
stop28
fi

if want 29; then
echo "== 29. a big library: the scan stays quick (a zip's CRC from its directory, each zip's system remembered)"
T=$(newroot t29)
mkdir -p "$T/root/roms/md"
$ROM "$T/one.md" ntsc >/dev/null
python3 - "$T/one.md" "$T/root/roms/md" <<'PY'
import os, random, sys, zipfile
rom = open(sys.argv[1], "rb").read()
random.seed(3)
words = "Sonic Streets Rage Golden Axe Shinobi Phantasy Star Ecco Gunstar Heroes Castle Vectorman Comix Zone".split()
for i in range(800):
    name = " ".join(random.sample(words, 3)) + " %04d (Hack)" % i
    with zipfile.ZipFile(os.path.join(sys.argv[2], name + ".zip"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(name + ".md", rom + bytes([i & 255, i >> 8]) * 512)
PY
scan_time() { python3 - "$1" <<'PY'
import re, sys
t = {}
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r'\[\s*([0-9.]+)\] \[(boot|games)\] (Genesis Plus GX PS5 |800 ROM)', line)
    if m: t.setdefault(m.group(2), float(m.group(1)))
print('%.2f' % (t['games'] - t['boot']) if 'boot' in t and 'games' in t else 'none')
PY
}
rc=$(run "$T" "0:0;100:$OPTIONS;102:0;110:$CROSS;112:0" "")
first=$(scan_time $T/root/logs/boot.log)
expect "[ $rc = 0 ] && grep -q '\[games\] 800 ROM(s)' $T/root/logs/boot.log" "800 zipped ROMs found"
expect "[ \$(grep -c '^sys:' $T/root/covers/crc-cache.txt) = 800 ] && [ \$(grep -c '\.zip	[0-9]*	[0-9]*	[0-9A-F]\{8\}	' $T/root/covers/crc-cache.txt) = 800 ]" "each zip's system and CRC remembered"
rc=$(run "$T" "0:0;100:$OPTIONS;102:0;110:$CROSS;112:0" "")
second=$(scan_time $T/root/logs/boot.log)
expect "python3 -c 'import sys; sys.exit(0 if float(\"$first\") < 4 and float(\"$second\") < 2 else 1)'" "first scan ${first} s (under 4), the next ${second} s (under 2)"
nosan "$T"
fi

# a press of each button in turn, from frame $1, 8 frames apart: SEQ 200 $L3R3 $DOWN $CROSS
SEQ() { local f=$1 out="" b; shift; for b in "$@"; do out="$out;$f:$b;$((f + 2)):0"; f=$((f + 8)); done; echo "${out#;}"; }
TONE="python3 $PWD/tests/check_tone.py"
CD="python3 $PWD/tests/make_test_cd.py"

if want 30; then
echo "== 30. CD audio through MD+ (bin/cue, wav, CHD): the tones come out at their pitch, both channels, clean"
# tests/make_test_rom.py mdplus asks the MegaSD for track 2 (looped); the disc's track 2 is 1000 Hz left, 1500 Hz right
for kind in cue wcue; do
	T=$(newroot t30$kind)
	$ROM "$T/root/roms/mdp.md" mdplus >/dev/null
	$CD "$T/root/roms/mdp.$kind" --loop >/dev/null
	rc=$(GENPLUS_HOST_AUDIO_DUMP=$T/audio.raw run "$T" "0:0;$(QUITAT 300)" "" "$T/root/roms/mdp.md")
	expect "[ $rc = 0 ]" "MD+ $kind: exit code 0 (got $rc)"
	expect "$TONE $T/audio.raw >$T/tone.txt" "MD+ $kind: 1000 Hz left, 1500 Hz right, in tune and clean"
	sed 's/^/    /' $T/tone.txt
	nosan "$T"
done
# a CHD beside the ROM: with "CD add-on: MegaSD (MD+)" it plays (chdman's big-endian audio read right)...
T=$(newroot t30chd)
echo "add_on=1" >>"$T/root/genplus-ps5.ini"
$ROM "$T/root/roms/mdp.md" mdplus >/dev/null
$CD "$T/root/roms/mdp.chd" >/dev/null
rc=$(GENPLUS_HOST_AUDIO_DUMP=$T/audio.raw run "$T" "0:0;$(QUITAT 300)" "" "$T/root/roms/mdp.md")
expect "[ $rc = 0 ] && $TONE $T/audio.raw >$T/tone.txt" "MD+ chd (add-on MegaSD): the tones, in tune and clean"
sed 's/^/    /' $T/tone.txt
nosan "$T"
# ...with Auto it is taken for an MSU-MD disc (Sega CD): no MD+ sound -- the reason for the setting
T=$(newroot t30auto)
$ROM "$T/root/roms/mdp.md" mdplus >/dev/null
$CD "$T/root/roms/mdp.chd" >/dev/null
rc=$(GENPLUS_HOST_AUDIO_DUMP=$T/audio.raw run "$T" "0:0;$(QUITAT 300)" "" "$T/root/roms/mdp.md")
expect "[ $rc = 0 ] && ! $TONE $T/audio.raw >/dev/null" "MD+ chd with add-on Auto: not played as MD+ (documented)"
# the setting's row: Triangle -> Settings, Up from Shader to the add-on row is not fixed; the ini key is checked instead
expect "grep -q '^add_on=1$' $T/../t30chd/root/genplus-ps5.ini" "add_on is kept in the settings file"
nosan "$T"
fi

if want 31; then
echo "== 31. Sega CD with a stand-in BIOS: cue and chd boot, the region's BIOS, the RAM cart saved with a .bak"
for kind in cue chd; do
	T=$(newroot t31$kind)
	mkdir -p "$T/root/bios"
	$ROM "$T/root/bios/bios_CD_U.bin" cdbios >/dev/null
	$CD "$T/root/roms/Test (USA).$kind" --region U >/dev/null
	rc=$(run "$T" "0:0;100:$CROSS;140:0;$(QUITAT 200)" "90,130" "$T/root/roms/Test (USA).$kind")
	expect "[ $rc = 0 ] && grep -q 'running \"Test (USA)\" (Sega CD)' $T/root/logs/boot.log" "$kind: the disc boots as a Sega CD game"
	expect "$CHECK $T/dump/flip00090.ppm 960 540 red >/dev/null && $CHECK $T/dump/flip00130.ppm 960 540 green >/dev/null" "$kind: the BIOS runs (red), the pad reaches it (green)"
	nosan "$T"
done
# a European disc with only the European BIOS
T=$(newroot t31e)
mkdir -p "$T/root/bios"
$ROM "$T/root/bios/bios_CD_E.bin" cdbios >/dev/null
$CD "$T/root/roms/Test (Europe).cue" --region E >/dev/null
rc=$(run "$T" "0:0;$(QUITAT 200)" "90" "$T/root/roms/Test (Europe).cue")
expect "[ $rc = 0 ] && grep -q 'running \"Test (Europe)\" (Sega CD)' $T/root/logs/boot.log && $CHECK $T/dump/flip00090.ppm 960 540 red >/dev/null" "a European disc boots with bios_CD_E.bin"
nosan "$T"
# the RAM cart: the "BIOS" writes \$42 to its first byte; the next run erases it (\$FF while Cross is held): the file
# follows, and the cart as it was is kept in cart.brm.bak
T=$(newroot t31b)
mkdir -p "$T/root/bios"
$ROM "$T/root/bios/bios_CD_U.bin" cdbios >/dev/null
$CD "$T/root/roms/Test (USA).cue" >/dev/null
rc=$(run "$T" "0:0;$(QUITAT 200)" "" "$T/root/roms/Test (USA).cue")
C=$T/root/saves/cart.brm
expect "[ -f $C ] && python3 -c 'import sys; sys.exit(0 if open(\"$C\",\"rb\").read()[0] == 0x42 else 1)'" "the RAM cart is saved"
expect "[ ! -e $C.bak ]" "no .bak while there was nothing to keep"
rc=$(run "$T" "0:0;20:$CROSS;$(QUITAT 200)" "" "$T/root/roms/Test (USA).cue")
expect "python3 -c 'import sys; sys.exit(0 if open(\"$C\",\"rb\").read()[0] == 0xFF and open(\"$C.bak\",\"rb\").read()[0] == 0x42 else 1)'" "the erase is saved; cart.brm.bak keeps the cart as it was"
nosan "$T"
fi

if want 32; then
echo "== 32. saves kept safe: the previous state, a damaged state refused, 10 save/load cycles, battery save .bak"
T=$(newroot t32)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
rc=$(run "$T" "0:0;100:$L2UP;105:0;150:$L2UP;155:0;$(QUITAT 200)" "" "$T/root/roms/test.md")
ST=$T/root/states/MegaDrive/test.state1
expect "[ $rc = 0 ] && [ -f $ST ] && [ -f $ST.prev ] && [ ! -e $ST.part ]" "a state saved over another keeps it as .prev"
# pause, Down 3 -> "Load the previous state (slot 1)", Cross
rc=$(run "$T" "0:0;$(SEQ 100 $L3R3 $DOWN $DOWN $DOWN $CROSS);$(QUITAT 200)" "" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && grep -q 'load state 1 (previous): ok' $T/root/logs/boot.log" "the pause menu loads the previous state"
nosan "$T"
# a damaged (cut) state: refused, the game goes on
head -c 1000 $ST >$ST.cut && mv $ST.cut $ST
rc=$(run "$T" "0:0;100:$L2DOWN;105:0;$(QUITAT 200)" "190" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && grep -q 'load state 1 (.*): 1000 bytes, .*refused' $T/root/logs/boot.log && $CHECK $T/dump/flip00190.ppm 960 540 red >/dev/null" "a damaged state is refused and the game goes on"
nosan "$T"
# 10 saves and loads in a row
T=$(newroot t32c)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
pad="0:0"
for i in $(seq 0 9); do f=$((100 + i * 30)); pad="$pad;$f:$L2UP;$((f + 5)):0;$((f + 15)):$L2DOWN;$((f + 20)):0"; done
rc=$(run "$T" "$pad;$(QUITAT 450)" "440" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && [ \$(grep -c 'load state 1: ok' $T/root/logs/boot.log) = 10 ] && $CHECK $T/dump/flip00440.ppm 960 540 red >/dev/null" "10 save/load cycles: all loaded, the game still runs"
nosan "$T"
# battery RAM: the save as it was before this session's first write is kept as .srm.bak
T=$(newroot t32s)
$ROM "$T/root/roms/save.md" sram >/dev/null
rc=$(run "$T" "0:0;$(QUITAT 100)" "" "$T/root/roms/save.md")
S=$T/root/saves/MegaDrive/save.srm
expect "[ -f $S ] && [ ! -e $S.bak ]" "a first battery save: nothing to keep"
rc=$(run "$T" "0:0;20:$CROSS;$(QUITAT 100)" "" "$T/root/roms/save.md")
expect "[ -f $S.bak ] && python3 -c 'import sys; sys.exit(0 if 0x42 in open(\"$S.bak\",\"rb\").read() and 0x42 not in open(\"$S\",\"rb\").read() else 1)'" "the game's erase is saved, the save before it kept as .srm.bak"
nosan "$T"
fi

if want 33; then
echo "== 33. settings for one game: kept in games/<system>/<game>.ini, the global ones untouched"
T=$(newroot t33)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
G=$T/root/games/MegaDrive/test.ini
# pause, Down 6 -> "Settings for this game", Cross: "Its own" (the menu stays), Circle: back to the game
rc=$(run "$T" "0:0;$(SEQ 100 $L3R3 $DOWN $DOWN $DOWN $DOWN $DOWN $DOWN $CROSS $CIRCLE);$(QUITAT 240)" "" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && [ -f $G ] && grep -q \"this game's own settings\" $G" "the game gets its own settings file"
# its own shader: this game is drawn through it, another game and the global file aren't changed
sed -i 's/^shader=.*/shader=3/' $G
$ROM "$T/root/roms/other.md" ntsc >/dev/null
rc=$(run "$T" "0:0;$(QUITAT 100)" "" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && grep -q \"this game's own settings\" $T/root/logs/boot.log && grep -q 'shader crt-lottes-fast' $T/root/logs/boot.log" "the game runs with its own shader"
expect "grep -q '^shader=0$' $T/root/genplus-ps5.ini && grep -q '^shader=3$' $G" "the global file keeps shader=0"
rc=$(run "$T" "0:0;$(QUITAT 100)" "" "$T/root/roms/other.md")
expect "[ $rc = 0 ] && ! grep -q 'shader crt-lottes-fast' $T/root/logs/boot.log" "another game uses the global settings"
# back to the global ones: the file goes
rc=$(run "$T" "0:0;$(SEQ 100 $L3R3 $DOWN $DOWN $DOWN $DOWN $DOWN $DOWN $CROSS $CIRCLE);$(QUITAT 240)" "" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && [ ! -e $G ] && grep -q '^shader=0$' $T/root/genplus-ps5.ini" "\"The global ones\" removes the game's file"
nosan "$T"
fi

if want 34; then
echo "== 34. save protection: no backup, no overwrite; a state kept or not replaced; checksums"
# the battery save can't be backed up (its .bak is a folder): the new save goes beside it, the old one untouched
T=$(newroot t34)
$ROM "$T/root/roms/save.md" sram >/dev/null
rc=$(run "$T" "0:0;$(QUITAT 100)" "" "$T/root/roms/save.md")
S=$T/root/saves/MegaDrive/save.srm
before=$(md5sum < $S)
mkdir -p "$S.bak/x"
rc=$(run "$T" "0:0;20:$CROSS;$(QUITAT 100)" "" "$T/root/roms/save.md")
expect "[ \"\$(md5sum < $S)\" = '$before' ] && [ -f $S.new ] && python3 -c 'import sys; sys.exit(0 if 0x42 not in open(\"$S.new\",\"rb\").read() else 1)'" "battery save: no backup -> the file is left alone, the new one is .srm.new"
nosan "$T"
# the Sega CD RAM cart, the same
T=$(newroot t34b)
mkdir -p "$T/root/bios"
$ROM "$T/root/bios/bios_CD_U.bin" cdbios >/dev/null
$CD "$T/root/roms/Test (USA).cue" >/dev/null
rc=$(run "$T" "0:0;$(QUITAT 200)" "" "$T/root/roms/Test (USA).cue")
C=$T/root/saves/cart.brm
mkdir -p "$C.bak/x"
rc=$(run "$T" "0:0;20:$CROSS;$(QUITAT 200)" "" "$T/root/roms/Test (USA).cue")
expect "python3 -c 'import sys; sys.exit(0 if open(\"$C\",\"rb\").read()[0] == 0x42 and open(\"$C.new\",\"rb\").read()[0] == 0xFF else 1)'" "RAM cart: no backup -> cart.brm left alone (the core's own write at unload too), the new one is .new"
nosan "$T"
# a state whose old one can't be kept as .prev: nothing replaced
T=$(newroot t34c)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
rc=$(run "$T" "0:0;100:$L2UP;105:0;$(QUITAT 150)" "" "$T/root/roms/test.md")
ST=$T/root/states/MegaDrive/test.state1
before=$(md5sum < $ST)
mkdir -p "$ST.prev/x"
rc=$(run "$T" "0:0;100:$L2UP;105:0;$(QUITAT 150)" "" "$T/root/roms/test.md")
expect "[ \"\$(md5sum < $ST)\" = '$before' ] && [ ! -e $ST.part ] && grep -q 'could not keep the previous state.*left as it was' $T/root/logs/boot.log && grep -q 'save state 1: failed' $T/root/logs/boot.log" "state: the old one can't be kept -> the slot is left as it was, the save says failed"
rm -rf "$ST.prev"
# a state changed inside (same size): its CRC-32 refuses it
python3 - "$ST" <<'PY'
import sys
p = sys.argv[1]; d = bytearray(open(p, 'rb').read()); d[1000] ^= 0x55; open(p, 'wb').write(d)
PY
rc=$(run "$T" "0:0;100:$L2DOWN;105:0;$(QUITAT 150)" "140" "$T/root/roms/test.md")
expect "grep -q 'load state 1 .*CRC-32 [0-9a-f]*, the file says [0-9a-f]*: refused' $T/root/logs/boot.log && $CHECK $T/dump/flip00140.ppm 960 540 red >/dev/null" "state: one byte changed -> refused by its CRC-32, the game goes on"
# a state of 1.8 (no trailer, the core's size): still loads
rc=$(run "$T" "0:0;100:$L2UP;105:0;$(QUITAT 150)" "" "$T/root/roms/test.md")
python3 - "$ST" <<'PY'
import sys
p = sys.argv[1]; d = open(p, 'rb').read(); assert d[-16:-12] == b'GPXS'; open(p, 'wb').write(d[:-16])
PY
rc=$(run "$T" "0:0;100:$L2DOWN;105:0;$(QUITAT 150)" "" "$T/root/roms/test.md")
expect "grep -q 'load state 1: ok (an older state without a checksum)' $T/root/logs/boot.log" "state: a 1.8 state (no checksum) still loads"
rc=$(run "$T" "0:0;100:$L2UP;105:0;120:$L2DOWN;125:0;$(QUITAT 150)" "" "$T/root/roms/test.md")
expect "grep -q 'load state 1: ok (checksum ok)' $T/root/logs/boot.log" "state: a new state loads with its checksum checked"
nosan "$T"
# 200 save/load cycles in one session, every one checked
T=$(newroot t34d)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
pad="0:0"
for i in $(seq 0 199); do f=$((100 + i * 12)); pad="$pad;$f:$L2UP;$((f + 3)):0;$((f + 6)):$L2DOWN;$((f + 9)):0"; done
rc=$(MAXFLIPS=9000 run "$T" "$pad;$(QUITAT 2600)" "2590" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && [ \$(grep -c 'load state 1: ok (checksum ok)' $T/root/logs/boot.log) = 200 ] && $CHECK $T/dump/flip02590.ppm 960 540 red >/dev/null" "200 save/load cycles: all 200 checked and loaded, the game still runs"
nosan "$T"
fi

if want 35; then
echo "== 35. languages: English by default; Spanish, Portuguese (Brazil), French and German translated"
expect "python3 $PWD/tests/check_i18n.py $PWD/frontend/fe_i18n.cpp >/dev/null" "every translation has its five languages and the English format specifiers"
for L in 1 2 3 4; do
	T=$(newroot t35_$L)
	echo "language=$L" >>"$T/root/genplus-ps5.ini"
	$ROM "$T/root/roms/Alpha (USA).md" ntsc >/dev/null
	mkdir -p "$T/root/cheats/MegaDrive" # the cheat manager's list (its empty screen: group 85)
	printf 'cheats = 1\ncheat0_desc = "Infinite lives"\ncheat0_code = "FFA3BF:0003"\ncheat0_enable = false\n' \
		>"$T/root/cheats/MegaDrive/Alpha (USA).cht"
	# the settings screen scrolled through, then a game's pause menu (the cheat manager on the way: Down seven times)
	pad="0:0;20:$TRIANGLE;22:0"; f=30
	for i in $(seq 1 48); do pad="$pad;$f:$DOWN;$((f + 2)):0"; f=$((f + 6)); done
	pad="$pad;$((f + 4)):$CIRCLE;$((f + 6)):0;$((f + 20)):$CROSS;$((f + 22)):0"; f=$((f + 120))
	pad="$pad;$f:$L3R3;$((f + 2)):0"; f=$((f + 10))
	for i in $(seq 1 7); do pad="$pad;$f:$DOWN;$((f + 2)):0"; f=$((f + 8)); done
	pad="$pad;$f:$CROSS;$((f + 2)):0;$((f + 20)):$CIRCLE;$((f + 22)):0"; f=$((f + 30))
	for i in $(seq 1 5); do pad="$pad;$f:$DOWN;$((f + 2)):0"; f=$((f + 8)); done
	pad="$pad;$((f + 10)):$UP;$((f + 12)):0;$((f + 20)):$CROSS;$((f + 22)):0"
	rm -f $T/missing.txt
	rc=$(GENPLUS_HOST_I18N_MISSING=$T/missing.txt run "$T" "$pad" "")
	# what was shown untranslated: only names (the game's, the chips', the buttons') and texts already translated
	python3 - $T/missing.txt $PWD/frontend/fe_i18n.cpp > $T/untranslated.txt <<'PY'
import re, sys
src = open(sys.argv[2], encoding='utf-8').read()
translated = set(re.findall(r'"((?:[^"\\]|\\.)*)"', src))
spec = re.compile(r'%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?[diouxXeEfgGcsp])')
# a text printed from a translated format: the format as a pattern
formats = [re.compile('^' + ''.join('.*' if spec.fullmatch(part) else re.escape(part)
                                    for part in re.split('(' + spec.pattern + ')', f)) + '$')
           for f in translated if '%' in f]
names = {'Alpha (USA)', 'A', 'X', 'Y', 'Z', 'Mode', 'L1', 'R1', 'OPTIONS', 'MAME YM2612', 'English', 'Español',
         'Português (Brasil)', 'Français', 'Deutsch', 'Off', 'S-Video', 'RGB', '4:3', '16:9', 'GPU', 'CPU'}
for line in open(sys.argv[1], encoding='utf-8'):
    t = line.rstrip('\n')
    if t in translated or t in names or not re.search('[A-Za-z]{2}', t) or re.fullmatch(r'[\d%. ]+', t):
        continue
    if any(f.match(t) for f in formats):
        continue
    if re.search(r'Mega Drive {3}MD {3}', t) or t.startswith('↣') or t.endswith(' 1.8') or re.search(r'\(\w+ \d\)$', t):
        continue
    print(t)
PY
	expect "[ $rc = 0 ] && [ ! -s $T/untranslated.txt ]" "$(sed -n 1p $T/root/genplus-ps5.ini >/dev/null; echo language $L): every menu text shown is translated ($(head -c 200 $T/untranslated.txt | tr '\n' '|'))"
	nosan "$T"
done
# the network screens (2.1): Settings, Network shares with a share that does not connect, Add a share, the keyboard on
# Server (Circle closes it), the share type switched to NFS, Circle asks to discard (Up, Cross: discard), Connect again;
# back to the shelf: R1 to its Network tab (the share's state), Square scans again, L1 back, quit
for L in 1 2 3 4; do
	T=$(newroot t35net_$L)
	echo "language=$L" >>"$T/root/genplus-ps5.ini"
	echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
	$ROM "$T/root/roms/Alpha (USA).md" ntsc >/dev/null
	key() { pad="$pad;$f:$1;$((f + 2)):0"; f=$((f + ${2:-6})); } # one press, then the next one $2 flips later
	pad="0:0;20:$TRIANGLE;22:0"; f=30
	for i in $(seq 1 8); do key $UP; done
	key $CROSS 20; key $DOWN; key $CROSS 20; key $CROSS 20; key $CIRCLE 20
	key $UP; key $RIGHT 20; key $CIRCLE 20; key $UP; key $CROSS 20
	key $DOWN; key $CROSS 20; key $CIRCLE 20; key $CIRCLE 30
	key 800 30; key 8000 60; key 400 30
	key $OPTIONS 10; key $CROSS 10
	rm -f $T/missing.txt
	rc=$(GENPLUS_HOST_REALTIME=1 GENPLUS_HOST_NETFS_DIR=$T/nothing GENPLUS_HOST_I18N_MISSING=$T/missing.txt run "$T" "$pad" "")
	python3 - $T/missing.txt $PWD/frontend/fe_i18n.cpp > $T/untranslated.txt <<'PY'
import re, sys
src = open(sys.argv[2], encoding='utf-8').read()
translated = set(re.findall(r'"((?:[^"\\]|\\.)*)"', src))
spec = re.compile(r'%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?[diouxXeEfgGcsp])')
formats = [re.compile('^' + ''.join('.*' if spec.fullmatch(part) else re.escape(part)
                                    for part in re.split('(' + spec.pattern + ')', f)) + '$')
           for f in translated if '%' in f]
# (the first walk's names and rules, and the network screens' names)
names = {'Alpha (USA)', 'A', 'X', 'Y', 'Z', 'Mode', 'L1', 'R1', 'OPTIONS', 'MAME YM2612', 'English', 'Español',
         'Português (Brasil)', 'Français', 'Deutsch', 'Off', 'S-Video', 'RGB', '4:3', '16:9', 'GPU', 'CPU',
         'SMB (Windows, Samba, NAS)', 'NFS', 'USB',
         'Mega Drive', 'Genesis', 'Sega CD', 'Mega-CD', 'Master System', 'Game Gear', 'SG-1000'}
try:
    lines = open(sys.argv[1], encoding='utf-8').read().splitlines()
except FileNotFoundError:
    lines = []
for t in lines:
    if t in translated or t in names or not re.search('[A-Za-z]{2}', t) or any(f.match(t) for f in formats):
        continue
    if re.search(r'Mega Drive {3}MD', t) or t.startswith('↣') or t.endswith(' 1.8') or re.search(r'\(\w+ \d\)$', t):
        continue
    print(t)
PY
	expect "[ $rc = 0 ] && grep -q 'network tab: scan again' $T/root/logs/boot.log && [ ! -s $T/untranslated.txt ]" "language $L: the network screens and the Network tab are translated ($(head -c 200 $T/untranslated.txt | tr '\n' '|'))"
	nosan "$T"
done
# English stays the default: no setting, English on the screen (the texts pass through as they are)
T=$(newroot t35en)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
rc=$(GENPLUS_HOST_I18N_MISSING=$T/missing.txt run "$T" "0:0;$(QUITAT 100)" "" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && [ ! -s $T/missing.txt ] && grep -q '^language=0$' $T/root/genplus-ps5.ini" "English by default (language=0): nothing is looked up"
fi

if want 36; then
echo "== 36. the GPU renderer (Vulkan, here on lavapipe): the picture, the shaders, the menus over the game"
T=$(newroot t36)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
rc=$(GENPLUS_HOST_GPU=1 run "$T" "0:0;100:$CROSS;140:0;$(QUITAT 200)" "90,130,205" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && grep -q 'renderer: GPU' $T/root/logs/boot.log" "the GPU renderer starts"
expect "$RECT $T/dump/flip00090.ppm $T/root/logs/boot.log red >/dev/null && $CHECK $T/dump/flip00130.ppm 960 540 green >/dev/null" "the picture where the log says, Cross -> green"
expect "python3 -c \"import sys; d=open('$T/dump/flip00205.ppm','rb').read().split(b'\\n',3); w=int(d[1].split()[0]); p=d[3]; r=p[(540*w+300)*3]; sys.exit(0 if 40 < r < 80 else 1)\"" "the pause menu over the game: the game at a quarter behind it, as on the CPU (premultiplied surface)"
expect "[ ! -e $T/root/logs/gpu-starting ]" "the start marker is removed once a frame is on screen"
nosan "$T"
# a start that died while the GPU was being set up: the next one uses the CPU
T=$(newroot t36b)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
mkdir -p $T/root/logs && touch $T/root/logs/gpu-starting
rc=$(GENPLUS_HOST_GPU=1 run "$T" "0:0;$(QUITAT 100)" "90" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && grep -q \"the last start with the GPU didn't reach the screen: this one uses the CPU\" $T/root/logs/boot.log && $CHECK $T/dump/flip00090.ppm 960 540 red >/dev/null" "after a failed GPU start the CPU draws, and the game runs"
# every GPU shader draws the game (single- and multi-pass: Guest Advanced, Royale, ScaleFX, the Mega Bezel...)
for s in $(seq 1 26); do
	T=$(newroot t36s$s)
	echo "shader=$s" >>"$T/root/genplus-ps5.ini"
	$ROM "$T/root/roms/test.md" ntsc >/dev/null
	# (the Mega Bezel's 30 passes run at about 1 frame a second on lavapipe, a CPU: its run stops at frame 40)
	if [ $s -ge 25 ]; then mf=40; at=35; quit=""; else mf=400; at=90; quit=";$(QUITAT 100)"; fi
	# (the shader built before the next frame, as 1.9 did: here the picture is checked at a set frame)
	rc=$(GENPLUS_HOST_GPU_SYNC_BUILD=1 GENPLUS_HOST_GPU=1 MAXFLIPS=$mf run "$T" "0:0$quit" "$at" "$T/root/roms/test.md")
	name=$(grep -o 'shader [^:]*: [0-9]* pass' $T/root/logs/boot.log | head -1)
	expect "{ [ $rc = 0 ] || [ $mf = 40 ]; } && [ -n \"$name\" ] && python3 -c \"import sys; d=open('$T/dump/flip000$at.ppm','rb').read().split(b'\\n',3); w=int(d[1].split()[0]); p=d[3]; sys.exit(0 if max(p[(y*w+960)*3] for y in range(440,640)) > 60 else 1)\"" "GPU $name: draws the game"
	expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "GPU shader $s: no sanitizer reports"
done
fi

if want 37; then
echo "== 37. the performance log: a row a second, CD audio seen, the sound/picture drift flat"
T=$(newroot t37)
echo "perf_log=1" >>"$T/root/genplus-ps5.ini"
$ROM "$T/root/roms/mdp.md" mdplus >/dev/null
$CD "$T/root/roms/mdp.cue" --loop >/dev/null
rc=$(run "$T" "0:0;$(QUITAT 1300)" "" "$T/root/roms/mdp.md")
P=$T/root/logs/perf-MegaDrive-mdp.csv
expect "[ $rc = 0 ] && [ -f $P ] && [ \$(grep -c '^[0-9]' $P) -ge 15 ]" "logs/perf-MegaDrive-mdp.csv: a row a second ($(grep -c '^[0-9]' $P 2>/dev/null) rows)"
expect "python3 - $P <<'PY'
import csv, sys
rows = [r for r in csv.DictReader(l for l in open(sys.argv[1]) if not l.startswith('#'))]
cd = [float(r['cd_audio_pct']) for r in rows[2:]]
drift = [float(r['av_drift_ms']) for r in rows]
sys.exit(0 if min(cd) > 90 and max(drift) - min(drift) < 5 else 1)
PY" "CD audio seen playing; the sound/picture drift stays within 5 ms over the run"
expect "grep -q '\[perf\] [0-9]* s, [0-9]* frames: core' $T/root/logs/boot.log" "a summary in boot.log when the game closes"
nosan "$T"
fi

if want 38; then
echo "== 38. 2.0: storage faults while a state is replaced; underruns; CHD read-ahead; GPU shader cache, parameters, your own presets, the shelf"
# the new state can't take its name: the old one is put back, untouched
T=$(newroot t38)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
rc=$(run "$T" "0:0;100:$L2UP;105:0;$(QUITAT 150)" "" "$T/root/roms/test.md")
ST=$T/root/states/MegaDrive/test.state1
before=$(md5sum < $ST)
rc=$(GENPLUS_HOST_FAIL_RENAME=from=state1.part run "$T" "0:0;100:$L2UP;105:0;$(QUITAT 150)" "" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && [ \"\$(md5sum < $ST)\" = '$before' ] && [ ! -e $ST.part ] && grep -q 'the old one is back, the slot is left as it was' $T/root/logs/boot.log" "fault: the new state can't take its name -> the old one is put back, unchanged"
# ... and the old one can't be put back either: it stays whole as .prev, the screen says so, the pause menu loads it
rc=$(GENPLUS_HOST_FAIL_RENAME=from=state1.part,from=state1.prev run "$T" "0:0;100:$L2UP;105:0;$(QUITAT 150)" "" "$T/root/roms/test.md")
expect "[ ! -e $ST ] && [ \"\$(md5sum < $ST.prev)\" = '$before' ] && grep -q 'nor the old one back.*whole in .*test.state1.prev' $T/root/logs/boot.log" "fault: nor the old one back -> it is whole in .prev, and the log says where"
rc=$(run "$T" "0:0;100:$L3R3;102:0;110:$DOWN;112:0;116:$DOWN;118:0;122:$DOWN;124:0;130:$CROSS;132:0;$(QUITAT 200)" "" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && grep -q 'load state 1 (previous): ok' $T/root/logs/boot.log" "the pause menu's Load the previous state brings it back"
# an empty slot whose new state can't take its name: stays empty, nothing left behind
T=$(newroot t38b)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
rc=$(GENPLUS_HOST_FAIL_RENAME=from=state1.part run "$T" "0:0;100:$L2UP;105:0;$(QUITAT 150)" "" "$T/root/roms/test.md")
ST=$T/root/states/MegaDrive/test.state1
expect "[ ! -e $ST ] && [ ! -e $ST.part ] && [ ! -e $ST.prev ] && grep -q 'the slot stays empty' $T/root/logs/boot.log" "fault on an empty slot: it stays empty, no stray file"
nosan "$T"
# underruns: time on the shelf (no game, no sound fed) is not counted
T=$(newroot t38c)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
rc=$(GENPLUS_HOST_REALTIME=1 run "$T" "0:0;$(SHELFQUIT_AT 200)" "")
expect "[ $rc = 0 ] && grep -q '\[audio\] closed, 0 underruns' $T/root/logs/boot.log" "3 s on the shelf: 0 underruns counted ($(grep -o 'closed, [0-9]* underruns' $T/root/logs/boot.log))"
# CHD: hunks decompressed ahead on their own thread
T=$(newroot t38d)
printf 'add_on=1\nperf_log=1\n' >>"$T/root/genplus-ps5.ini"
$ROM "$T/root/roms/mdp.md" mdplus >/dev/null
$CD "$T/root/roms/mdp.chd" --loop >/dev/null
rc=$(run "$T" "0:0;$(QUITAT 900)" "" "$T/root/roms/mdp.md")
expect "[ $rc = 0 ] && python3 - $T/root/logs/boot.log <<'PY'
import re, sys
m = re.search(r'\[perf\] CHD: (\d+) hunk\(s\) read, (\d+) decompressed ahead', open(sys.argv[1]).read())
sys.exit(0 if m and int(m.group(1)) >= 10 and int(m.group(2)) >= int(m.group(1)) * 3 // 4 else 1)
PY" "CHD: most hunks decompressed ahead ($(grep -o 'CHD: [0-9]* hunk.*' $T/root/logs/boot.log | head -1))"
nosan "$T"
# GPU: a shader built in the background (the plain picture and a message meanwhile), the pipeline cache saved
T=$(newroot t38e)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
sed -i 's/^shader=.*/shader=1/' $T/root/genplus-ps5.ini
rc=$(GENPLUS_HOST_GPU=1 run "$T" "0:0;$(QUITAT 100)" "" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && grep -q 'built in the background' $T/root/logs/boot.log && [ -s $T/root/cache/gpu-pipelines.bin ]" "GPU: the shader built in the background, the pipeline cache saved (cache/gpu-pipelines.bin)"
rc=$(GENPLUS_HOST_GPU=1 run "$T" "0:0;$(QUITAT 100)" "" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && grep -q 'pipeline cache: [0-9]* KiB from' $T/root/logs/boot.log" "GPU: the next start reads the cache back"
# Settings -> Shader parameters: Right three times on the first one, kept in shader-params.ini, read at the next start
P="0:0;60:$L3R3;62:0"; f=70
for i in 1 2 3 4 5; do P="$P;$f:$DOWN;$((f + 2)):0"; f=$((f + 6)); done
P="$P;$f:$CROSS;$((f + 2)):0"; f=$((f + 10)); P="$P;$f:$DOWN;$((f + 2)):0"; f=$((f + 6)); P="$P;$f:$CROSS;$((f + 2)):0"; f=$((f + 10))
for i in 1 2 3; do P="$P;$f:$RIGHT;$((f + 2)):0"; f=$((f + 6)); done
SHOT=$f; f=$((f + 4)); P="$P;$f:$CIRCLE;$((f + 2)):0"; f=$((f + 10)); P="$P;$f:$CIRCLE;$((f + 2)):0"; f=$((f + 10)); P="$P;$f:$CIRCLE;$((f + 2)):0"
rc=$(GENPLUS_HOST_GPU=1 run "$T" "$P;$(QUITAT $((f + 30)))" "$SHOT" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && grep -q '^crt-easymode	SHARPNESS_H	0.65$' $T/root/shader-params.ini" "Shader parameters: Sharpness Horizontal 0.50 -> 0.65 (three steps), saved"
rc=$(GENPLUS_HOST_GPU=1 run "$T" "0:0;$(QUITAT 100)" "" "$T/root/roms/test.md")
expect "grep -q '1 shader parameter(s) set in the menu' $T/root/logs/boot.log" "... and read back at the next start"
nosan "$T"
# your own presets: <root>/shaders/*.slangp compiled on the console when picked; a broken one says why, the game goes on
T=$(newroot t38f)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
mkdir -p $T/root/shaders/mine
cp -r shaders/slang/crt/crt-easymode.slangp shaders/slang/crt/shaders $T/root/shaders/mine/ 2>/dev/null
mkdir -p $T/root/shaders/mine/shaders && cp shaders/slang/crt/shaders/crt-easymode.slang $T/root/shaders/mine/shaders/
printf 'shaders = 1\nshader0 = broken.slang\n' >$T/root/shaders/broken.slangp
printf '#version 450\n#pragma stage vertex\nvoid main() { gl_Position = vec4(0); this does not compile }\n#pragma stage fragment\nlayout(location = 0) out vec4 F;\nvoid main() { F = vec4(1); }\n' >$T/root/shaders/broken.slang
n=$(grep -c '^[^#_].* = ' shaders/presets.txt)
# the shader list: the built-in ones, then broken (a lone .slang: listed too), broken.slangp, mine/crt-easymode
sed -i "s/^shader=.*/shader=$((n + 3))/" $T/root/genplus-ps5.ini
rc=$(GENPLUS_HOST_GPU=1 run "$T" "0:0;$(QUITAT 120)" "100" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && grep -q 'your preset mine/crt-easymode: 1 pass(es)' $T/root/logs/boot.log && grep -q 'shader mine/crt-easymode: 1 pass(es)' $T/root/logs/boot.log" "your own preset (shaders/mine/crt-easymode.slangp): compiled on the console, used"
expect "python3 -c \"import sys; d=open('$T/dump/flip00100.ppm','rb').read().split(b'\\n',3); w=int(d[1].split()[0]); p=d[3]; sys.exit(0 if max(p[(y*w+960)*3] for y in range(440,640)) > 60 else 1)\"" "... and it draws the game"
sed -i "s/^shader=.*/shader=$((n + 2))/" $T/root/genplus-ps5.ini
rc=$(GENPLUS_HOST_GPU=1 run "$T" "0:0;$(QUITAT 120)" "100" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && grep -q 'your preset broken (.*) could not be used: pass 0' $T/root/logs/boot.log && grep -q 'could not be set up: the plain picture' $T/root/logs/boot.log && $CHECK $T/dump/flip00100.ppm 960 540 red >/dev/null" "a broken preset of your own: the log says why, the plain picture, the game goes on"
nosan "$T"
# the shelf drawn by the GPU: the same picture as the CPU's
T=$(newroot t38g)
mkdir -p $T/root/roms/MegaDrive $T/root/covers
for g in Alpha Bravo Charlie; do $ROM "$T/root/roms/MegaDrive/$g.md" ntsc >/dev/null; done
python3 - $T/root/covers <<'PY'
import sys, zlib, struct
def png(path, w, h, f):
    raw = b''.join(b'\0' + bytes(c for x in range(w) for c in f(x, y)) for y in range(h))
    ch = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))
for i, n in enumerate(['Alpha', 'Bravo', 'Charlie']):
    png('%s/%s.png' % (sys.argv[1], n), 180, 256, lambda x, y, i=i: ((x * 255 // 180) if i != 1 else 40, y, 200 if (x // 20 + y // 20) % 2 else 60))
PY
# (in real time: the backdrop follows the selection after a moment, and fades in)
rc=$(GENPLUS_HOST_REALTIME=1 GENPLUS_HOST_GPU=1 run "$T" "0:0;$(SHELFQUIT_AT 160)" "150")
mv $T/dump/flip00150.ppm $T/gpu.ppm 2>/dev/null
expect "[ $rc = 0 ] && grep -q 'shelf\] drawn by the GPU' $T/root/logs/boot.log" "the shelf: drawn by the GPU when the GPU renders"
expect "grep -q 'shelf self-test (format [0-9]*): flat quad [0-9]*,[0-9]*,2[0-9][0-9] (want blue), perspective quad 2[0-9][0-9],' $T/root/logs/boot.log && grep -q 'gpu\] shelf scene: [0-9]* quad(s) drawn' $T/root/logs/boot.log" "the shelf's self-test at start: the quads come out (flat and in perspective), and the log says what is drawn"
rc=$(GENPLUS_HOST_REALTIME=1 run "$T" "0:0;$(SHELFQUIT_AT 160)" "150")
expect "python3 - $T/gpu.ppm $T/dump/flip00150.ppm <<'PY'
import sys
def load(p):
    d = open(p, 'rb').read().split(b'\n', 3); return d[3]
a, b = load(sys.argv[1]), load(sys.argv[2])
diff = sum(1 for i in range(0, len(a), 3 * 7) if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 60)
sys.exit(0 if diff * 7 * 3 < len(a) * 0.01 else 1)
PY" "... the same picture as the CPU's (under 1% of pixels differ)"
nosan "$T"
fi

# ---- network shares and the shelf's storage tabs (2.1, from Snes9x PS5: thatboialex's pull request #3) ----
# host/netfs_dirbackend.cpp turns every address in shares.txt into a folder on this machine (GENPLUS_HOST_NETFS_DIR);
# the app is the real one. The shelf looks for a connected share twice a second of wall time, so these runs are
# paced (GENPLUS_HOST_REALTIME). netfs's own unit tests (test-netfs here; test-nfs, test-tabs and smb-test in the
# Makefile) need no app.
L1=400; R1=800; SQUARE=8000

if want 39; then
echo "== 39. netfs: share addresses and redaction, routing, the libc --wrap layer, minizip and zlib on a share, the watchdog (in-memory share)"
NETFS_OUT=$(ASAN_OPTIONS=detect_leaks=1 timeout 120 "$PWD/build/host/test-netfs" 2>&1)
NETFS_RC=$?
echo "$NETFS_OUT" | tail -3 | sed 's/^/  /'
expect "[ $NETFS_RC = 0 ]" "netfs: every check passes"
expect "! echo \"\$NETFS_OUT\" | grep -q 'runtime error\|Sanitizer'" "no sanitizer reports"
fi

if want 40; then
echo "== 40. games on a share: on the shelf, a cartridge started from it, Sega CD discs (cue, chd) and MD+ CD audio streamed from it"
# a. a Mega Drive cartridge on an SMB share: listed, started from the shelf (read whole), its save state on the console
T=$(newroot t40)
mkdir -p "$T/share/MegaDrive"
$ROM "$T/share/MegaDrive/Net Game (USA).md" ntsc >/dev/null
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
rc=$(GENPLUS_HOST_REALTIME=1 MAXFLIPS=900 GENPLUS_HOST_NETFS_DIR="$T/share" run "$T" "0:0;150:$CROSS;152:0;280:$CROSS;320:0;340:$L2UP;345:0;$(QUITAT 400)" "300")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '\[net\] 1 share(s) in shares.txt' $T/root/logs/boot.log" "shares.txt was read"
expect "grep -q '\[net\] Connected to smb://10.0.0.5/games' $T/root/logs/boot.log" "the share connected"
expect "grep -q 'loading /smb/10.0.0.5/games/MegaDrive/Net Game (USA).md' $T/root/logs/boot.log && grep -q 'running \"Net Game (USA)\" (Mega Drive)' $T/root/logs/boot.log" "the game was started from the share"
expect "$CHECK $T/dump/flip00300.ppm 960 540 green >/dev/null" "it runs (Cross -> green)"
expect "[ -f '$T/root/states/MegaDrive/Net Game (USA).state1' ] && [ ! -e '$T/share/states' ] && [ \$(find $T/share -type f | wc -l) = 1 ]" "its state is on the console, nothing is written to the share"
expect "grep -q 'last_rom=/smb/10.0.0.5/games/MegaDrive/Net Game (USA).md' $T/root/genplus-ps5.ini" "the shelf remembers it"
nosan "$T"
# b. Sega CD discs on an NFS share (the BIOS on the console): the core reads the disc through netfs while it runs
for kind in cue chd; do
	T=$(newroot t40$kind)
	mkdir -p "$T/root/bios" "$T/share/SegaCD"
	$ROM "$T/root/bios/bios_CD_U.bin" cdbios >/dev/null
	$CD "$T/share/SegaCD/Test (USA).$kind" --region U >/dev/null
	echo "nfs://10.0.0.5/export/sega" >"$T/root/shares.txt"
	rc=$(GENPLUS_HOST_REALTIME=1 MAXFLIPS=900 GENPLUS_HOST_NETFS_DIR="$T/share" run "$T" "0:0;150:$CROSS;152:0;300:$CROSS;340:0;$(QUITAT 400)" "290,330")
	expect "[ $rc = 0 ] && grep -q 'loading /nfs/10.0.0.5/export/sega/SegaCD/Test (USA).$kind' $T/root/logs/boot.log && grep -q 'running \"Test (USA)\" (Sega CD)' $T/root/logs/boot.log" "$kind on a share: the disc boots as a Sega CD game"
	expect "$CHECK $T/dump/flip00290.ppm 960 540 red >/dev/null && $CHECK $T/dump/flip00330.ppm 960 540 green >/dev/null" "$kind on a share: the BIOS runs (red), the pad reaches it (green)"
	nosan "$T"
done
# c. CD audio while the game runs: an MD+ cartridge and its disc on the share, track 2 streamed (netfs's blocks and read-ahead)
for kind in cue chd; do
	T=$(newroot t40mdp$kind)
	mkdir -p "$T/share"
	[ $kind = chd ] && echo "add_on=1" >>"$T/root/genplus-ps5.ini"
	$ROM "$T/share/mdp.md" mdplus >/dev/null
	$CD "$T/share/mdp.$kind" --loop >/dev/null
	echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
	rc=$(GENPLUS_HOST_AUDIO_DUMP=$T/audio.raw GENPLUS_HOST_REALTIME=1 MAXFLIPS=900 GENPLUS_HOST_NETFS_DIR="$T/share" run "$T" "0:0;150:$CROSS;152:0;$(QUITAT 500)" "")
	expect "[ $rc = 0 ] && $TONE $T/audio.raw >$T/tone.txt" "MD+ $kind on a share: 1000 Hz left, 1500 Hz right, in tune and clean"
	sed 's/^/    /' $T/tone.txt
	nosan "$T"
done
# the same with 200 ms on every read, as a slow link: the game waits for the block it seeks to (logged), then plays on
T=$(newroot t40slow)
mkdir -p "$T/share"
$ROM "$T/share/mdp.md" mdplus >/dev/null
$CD "$T/share/mdp.cue" --loop >/dev/null
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
rc=$(GENPLUS_HOST_NETFS_DELAY_MS=200 GENPLUS_HOST_AUDIO_DUMP=$T/audio.raw GENPLUS_HOST_REALTIME=1 MAXFLIPS=900 GENPLUS_HOST_NETFS_DIR="$T/share" run "$T" "0:0;150:$CROSS;152:0;$(QUITAT 500)" "")
expect "[ $rc = 0 ] && grep -q 'smb://10.0.0.5/games: waited for mdp.bin in realtime (#1)' $T/root/logs/boot.log" "a slow link: the wait for the disc is logged"
expect "$TONE $T/audio.raw >$T/tone.txt" "and the track still plays at its pitch (after a gap while it waited)"
sed 's/^/    /' $T/tone.txt
nosan "$T"
# d. a share that does not connect: the shelf still works and says why; a game on it that is gone says so too
T=$(newroot t40f)
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
rc=$(GENPLUS_HOST_REALTIME=1 GENPLUS_HOST_NETFS_DIR="$T/nothing" run "$T" "0:0;100:$OPTIONS;102:0;110:$CROSS;112:0" "")
expect "[ $rc = 0 ] && grep -q '\[games\] 0 ROM(s)' $T/root/logs/boot.log" "a share that does not connect leaves the shelf working"
expect "grep -q '\[net\] The folder games is not on 10.0.0.5' $T/root/logs/boot.log" "and says why"
nosan "$T"
# e. shares.txt: written with its comments when missing; a bad line is named in the log, never its text
T=$(newroot t40n)
rc=$(run "$T" "0:0;$(SHELFQUIT_AT 40)" "")
expect "[ $rc = 0 ] && grep -q '^# Genesis Plus GX PS5 network shares' $T/root/shares.txt && ! grep -q '^smb://\|^nfs://' $T/root/shares.txt" "a commented shares.txt is written when there is none"
printf 'smb://bob@10.0.0.5/games?password=Hunter2x\nftp://10.0.0.5/x?password=Hunter2y\n' >"$T/root/shares.txt"
rc=$(GENPLUS_HOST_NETFS_DIR="$T/nothing" run "$T" "0:0;$(SHELFQUIT_AT 40)" "")
expect "[ $rc = 0 ] && grep -q 'shares.txt line 2: ' $T/root/logs/boot.log" "a line that is not an address is named by its number"
expect "! grep -rq 'Hunter2' $T/root/logs $T/out.txt" "no password in a log or on stdout"
nosan "$T"
fi

if want 41; then
echo "== 41. a game on a share with a made-up file name is named by its CRC in the background, its cover follows; a zip's system too"
T=$(newroot t41)
MDREPO=Sega_-_Mega_Drive_-_Genesis
mkdir -p "$T/srv/$MDREPO/Named_Boxarts" "$T/share/misc"
python3 - "$T/srv/$MDREPO/Named_Boxarts" <<'PY'
import sys
from PIL import Image
Image.new('RGB', (512, 357), (0, 255, 0)).save(sys.argv[1] + '/Streets of Rage 2 (Europe).png')
open(sys.argv[1] + '/Streets of Rage 2 (USA).png', 'w').write('Streets of Rage 2 (Europe).png')
PY
$ROM "$T/share/sor2.md" ntsc >/dev/null
python3 tests/forge_crc.py "$T/share/sor2.md" $(grep -P "^md\t[0-9A-F]{8}\tStreets of Rage 2 \(USA\)$" frontend/data/nointro.tsv | cut -f2) >/dev/null
# a Game Gear game in a zip, in a folder whose name says nothing: listed as a Mega Drive game until its zip is read
$ROM "$T/gg.gg" >/dev/null
python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1],'w'); z.write(sys.argv[2],'Pocket.gg'); z.close()" "$T/share/misc/pocket.zip" "$T/gg.gg"
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
PORT=18082
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
sleep 1
rc=$(OFFLINE= COVER_URL="http://127.0.0.1:$PORT/\${repo}/Named_Boxarts/\${name}.png" GENPLUS_HOST_REALTIME=1 MAXFLIPS=420 GENPLUS_HOST_NETFS_DIR="$T/share" \
	run "$T" "0:0" "400")
kill $SRVPID 2>/dev/null
expect "grep -q '\[games\] naming 2 network ROM(s) by CRC' $T/root/logs/boot.log" "the pass was started for the two games the scan did not read"
expect "grep -q 'sor2.md \[md\] -> \"Streets of Rage 2 (USA)\" (by CRC)' $T/root/logs/boot.log" "the next scan names the cartridge by its CRC"
expect "grep -q 'misc/pocket.zip \[md\]' $T/root/logs/boot.log && grep -q 'misc/pocket.zip \[gg\]' $T/root/logs/boot.log" "the zip moves to the Game Gear once its directory was read"
expect "grep -q '/smb/10.0.0.5/games/sor2.md' $T/root/covers/crc-cache.txt && grep -q '^sys:/smb/10.0.0.5/games/misc/pocket.zip' $T/root/covers/crc-cache.txt" "both are kept for the next start"
expect "[ -f '$T/root/covers/MegaDrive/Streets of Rage 2 (USA).png' ]" "the cover of the game named late was downloaded"
nosan "$T"
# the next start reads nothing over the network: the cache names both
rc=$(GENPLUS_HOST_REALTIME=1 MAXFLIPS=200 GENPLUS_HOST_NETFS_DIR="$T/share" run "$T" "0:0" "")
expect "[ \$(grep -c 'sor2.md \[md\] -> \"Streets of Rage 2 (USA)\" (by CRC)' $T/root/logs/boot.log) -ge 1 ] && grep -q 'misc/pocket.zip \[gg\]' $T/root/logs/boot.log && ! grep -q 'naming .* network ROM' $T/root/logs/boot.log" "the next start: named from the cache, no pass needed"
nosan "$T"
fi

if want 42; then
echo "== 42. the Network shares screen: add, change, test and remove a share with the on-screen keyboard (Settings, Network shares)"
# tests/osk_pad.py turns the steps into a pad script (the keyboard's layout is its model). Settings is Triangle on the shelf; Up eight
# times from Shader reaches Network shares (Back, Updates, Debug logs, Performance log, Renderer, Language, Download all borders,
# Network shares). Every run
# ends: Circle twice, OPTIONS, Cross.
net_pad() { python3 tests/osk_pad.py 2>/dev/null; }
net_quit='press CIRCLE 2
press OPTIONS
wait 8
press CROSS'
# a. a new share: server, share, folder with a space, user, a password that needs Shift and both kinds of symbols
T=$(newroot t42)
mkdir -p "$T/share"
PAD=$(net_pad <<STEPS
frame 40
press TRIANGLE
press UP 8
press CROSS
press CROSS
field 10.0.0.5
press DOWN
field games
press DOWN
field Mega Drive
press DOWN
field jason
press DOWN
field Zq9@pw&1
press DOWN 6
press CROSS
wait 20
$net_quit
STEPS
)
rc=$(GENPLUS_HOST_NETFS_DIR="$T/share" run "$T" "$PAD" "")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "[ \"\$(tail -n 1 $T/root/shares.txt)\" = 'smb://jason@10.0.0.5/games/Mega%20Drive?password=Zq9%40pw%261' ]" "the share is written as one escaped line"
expect "grep -q '^# Genesis Plus GX PS5 network shares' $T/root/shares.txt" "after the comments the app wrote"
expect "grep -q '\[net\] shares.txt: saved smb://jason@10.0.0.5/games/Mega%20Drive' $T/root/logs/boot.log" "the log says what was saved"
expect "grep -q '\[net\] Connected to smb://jason@10.0.0.5/games/Mega%20Drive' $T/root/logs/boot.log" "and the share connects at once, without a restart"
expect "! grep -rqE 'Zq9|pw%26' $T/root/logs $T/out.txt" "the password is in no log and not on stdout"
nosan "$T"
# b. change one share (its note stays), test the connection, remove the other
T=$(newroot t42b)
mkdir -p "$T/share/Sub" && echo x >"$T/share/a.md"
printf '# my shares\nsmb://old/x   # my note\nnfs://nas/export/sega\n' >"$T/root/shares.txt"
PAD=$(net_pad <<STEPS
frame 40
press TRIANGLE
press UP 8
press CROSS
press CROSS
press DOWN 3
field bob
press DOWN 6
press CROSS
wait 120
press DOWN
press CROSS
wait 20
press DOWN
press CROSS
press DOWN 7
press CROSS
wait 8
press UP
press CROSS
wait 20
$net_quit
STEPS
)
rc=$(GENPLUS_HOST_REALTIME=1 GENPLUS_HOST_NETFS_DIR="$T/share" run "$T" "$PAD" "")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "[ \"\$(cat $T/root/shares.txt)\" = \"\$(printf '# my shares\nsmb://bob@old/x   # my note')\" ]" "one line changed (note kept), the other was removed, the comment stays"
expect "grep -q '\[net\] test of smb://bob@old/x: Connected. The folder has 1 file and 1 folder.' $T/root/logs/boot.log" "Test connection reports what is in the folder"
expect "grep -q '\[net\] shares.txt: removed nfs://nas/export/sega' $T/root/logs/boot.log" "the removal is logged"
nosan "$T"
# c. Save with nothing filled in changes nothing
T=$(newroot t42c)
PAD=$(net_pad <<STEPS
frame 40
press TRIANGLE
press UP 8
press CROSS
press CROSS
press DOWN 10
press CROSS
wait 8
press CIRCLE
$net_quit
STEPS
)
rc=$(run "$T" "$PAD" "")
expect "[ $rc = 0 ] && ! grep -q '^smb://\|^nfs://' $T/root/shares.txt" "an empty form is not saved (and Circle on a form with no changes leaves it at once)"
nosan "$T"
fi

if want 43; then
echo "== 43. shelf tabs: Internal and Network apart, L1 / R1 between them, the last tab kept, Square on Network scans again; L2 / R2 skip 10"
# The tab bar sits at the top: the open tab's pill is the accent colour (78,62,170), the other one dark (32,30,58); (795,30) is
# inside the Internal pill and (1120,30) inside the Network pill (above their names). The share connects within a few seconds of wall time.
tab_pills() { # dump the active tab: Internal | Network
	local f=$1 on=$2
	if [ "$on" = Internal ]; then
		$CHECK "$f" 795 30 78 62 170 >/dev/null && $CHECK "$f" 1120 30 32 30 58 >/dev/null
	else
		$CHECK "$f" 795 30 32 30 58 >/dev/null && $CHECK "$f" 1120 30 78 62 170 >/dev/null
	fi
}
T=$(newroot t43)
mkdir -p "$T/share"
$ROM "$T/root/roms/Local Game (USA).md" ntsc >/dev/null
$ROM "$T/share/Net Game (USA).md" ntsc >/dev/null
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
rc=$(GENPLUS_HOST_REALTIME=1 GENPLUS_HOST_NETFS_DIR="$T/share" run "$T" "0:0;220:$R1;222:0;300:$SQUARE;302:0;380:$L1;382:0;420:$OPTIONS;422:0;430:$CROSS;432:0" "200,260,400")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "tab_pills $T/dump/flip00200.ppm Internal" "the shelf opens on the Internal tab"
expect "tab_pills $T/dump/flip00260.ppm Network" "R1 goes to the Network tab"
expect "tab_pills $T/dump/flip00400.ppm Internal" "L1 comes back"
expect "grep -q '\[shelf\] tab Network: 1 game' $T/root/logs/boot.log" "the Network tab holds the game on the share"
expect "grep -q '\[shelf\] tab Internal: 1 game' $T/root/logs/boot.log" "and the Internal tab only the local one"
expect "grep -q '\[shelf\] network tab: scan again\$' $T/root/logs/boot.log" "Square on the Network tab scans again"
expect "[ \$(grep -c '\[games\] 2 ROM' $T/root/logs/boot.log) -ge 2 ]" "and the scan found both games again"
expect "grep -q 'last_tab=0' $T/root/genplus-ps5.ini" "the tab left on is kept (Internal)"
nosan "$T"
T=$(newroot t43b)
mkdir -p "$T/share"
$ROM "$T/root/roms/Local Game (USA).md" ntsc >/dev/null
$ROM "$T/share/Net Game (USA).md" ntsc >/dev/null
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
echo "last_tab=2" >>"$T/root/genplus-ps5.ini"
rc=$(GENPLUS_HOST_REALTIME=1 GENPLUS_HOST_NETFS_DIR="$T/share" run "$T" "0:0;300:$OPTIONS;302:0;310:$CROSS;312:0" "40,250")
expect "[ $rc = 0 ] && tab_pills $T/dump/flip00040.ppm Network" "with last_tab=2 and a share set up the shelf opens on Network, before the share is up"
expect "tab_pills $T/dump/flip00250.ppm Network" "and stays there when its games arrive"
expect "grep -q 'last_tab=2' $T/root/genplus-ps5.ini" "and keeps the tab on quit"
T=$(newroot t43c)
$ROM "$T/root/roms/Local Game (USA).md" ntsc >/dev/null
echo "last_tab=2" >>"$T/root/genplus-ps5.ini"
rc=$(run "$T" "0:0;60:$R1;62:0;70:$SQUARE;72:0;90:$L1;92:0;120:$OPTIONS;122:0;130:$CROSS;132:0" "40,80,110")
expect "[ $rc = 0 ] && tab_pills $T/dump/flip00040.ppm Internal" "last_tab=2 without a share set up opens on the games instead"
expect "tab_pills $T/dump/flip00080.ppm Network && grep -q '\[shelf\] tab Network: 0 game' $T/root/logs/boot.log" "R1 shows the empty Network tab"
expect "grep -q '\[shelf\] network tab: scan again\$' $T/root/logs/boot.log" "Square on it scans again, with no share to ask"
expect "tab_pills $T/dump/flip00110.ppm Internal && grep -q 'last_tab=0' $T/root/genplus-ps5.ini" "L1 goes back and the tab is kept"
nosan "$T"
T=$(newroot t43d)
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
rc=$(GENPLUS_HOST_REALTIME=1 GENPLUS_HOST_NETFS_DIR="$T/nothing" run "$T" "0:0;150:$SQUARE;152:0;330:$OPTIONS;332:0;340:$CROSS;342:0" "100")
expect "[ $rc = 0 ] && tab_pills $T/dump/flip00100.ppm Network" "a share that is down leaves a shelf with no games on the Network tab"
expect "grep -q '\[shelf\] network tab: scan again, reconnecting' $T/root/logs/boot.log" "Square connects it again"
expect "[ \$(grep -c 'The folder games is not on 10.0.0.5' $T/root/logs/boot.log) -ge 2 ]" "and says why each time"
nosan "$T"
# the system tabs (Up / Down) work inside the storage tab; L2 / R2 skip 10 games (L1 / R1 did before 2.1)
T=$(newroot t43e)
mkdir -p "$T/share"
for i in $(seq -w 1 12); do $ROM "$T/root/roms/Game $i (USA).md" ntsc >/dev/null; done
$ROM "$T/root/roms/Pocket (USA).gg" >/dev/null
$ROM "$T/share/Net Pocket (USA).gg" >/dev/null
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
# (back to the shelf from a game: the pause menu, Up twice, Cross)
rc=$(GENPLUS_HOST_REALTIME=1 MAXFLIPS=900 GENPLUS_HOST_NETFS_DIR="$T/share" run "$T" "0:0;150:$R2;152:0;170:$CROSS;172:0;$(SEQ 230 $L3R3 $UP $UP $CROSS);320:$R1;322:0;340:$DOWN;342:0;380:$CROSS;382:0;$(QUITAT 480)" "")
expect "[ $rc = 0 ] && grep -q 'loading .*/Game 11 (USA).md' $T/root/logs/boot.log" "R2 skips 10 games (Game 01 -> Game 11)"
expect "grep -q '\[shelf\] tab Game Gear: 1 game' $T/root/logs/boot.log && grep -q 'loading /smb/10.0.0.5/games/Net Pocket (USA).gg' $T/root/logs/boot.log" "on the Network tab, Down shows its Game Gear game, which runs"
nosan "$T"
fi

# ---- the cheats (2.1): libretro .cht files, the cheat manager, the reload when the file changes -------------------------
SQUARE=8000; L1=400; R1=800
CHEATS_TEST=$PWD/build/host/cheats-test
# the pause menu's Cheat manager (Down seven times), opened at frame $1
CHEATMENU_AT() { local f=$1 i out="$1:$L3R3;$(($1 + 2)):0"; f=$((f + 10)); for i in 1 2 3 4 5 6 7; do out="$out;$f:$DOWN;$((f + 2)):0"; f=$((f + 8)); done; echo "$out;$f:$CROSS;$((f + 2)):0"; }
PRESS() { echo "$1:$2;$(($1 + 2)):0"; } # frame button
# the codes, built for an address of the test ROMs: Mega Drive Game Genie (address, value), Master System Game Genie
# (address, value, the byte it replaces); checked against the core's own decoder (cheats-test decode)
ggmd() { python3 - "$1" "$2" <<'PY'
import sys
a, d = int(sys.argv[1], 16), int(sys.argv[2], 16)
n = [(d >> 3) & 31, ((d & 7) << 2) | ((a >> 14) & 3), (a >> 9) & 31, ((a >> 20) & 15) | (((a >> 8) & 1) << 4),
     ((d >> 12) & 1) | (((a >> 16) & 15) << 1), ((d >> 15) & 1) | (((d >> 8) & 15) << 1), (((d >> 13) & 3) << 3) | ((a >> 5) & 7), a & 31]
g = "ABCDEFGHJKLMNPRSTVWXYZ0123456789"
print("".join(g[x] for x in n[:4]) + "-" + "".join(g[x] for x in n[4:]))
PY
}
ggsms() { python3 - "$1" "$2" "$3" <<'PY'
import sys
a, d, r = int(sys.argv[1], 16), int(sys.argv[2], 16), int(sys.argv[3], 16)
x = r ^ 0xBA
x = ((x << 2) | (x >> 6)) & 0xFF
print("%02X%X-%02X%X-%X0%X" % (d, (a >> 8) & 15, a & 0xFF, ((a >> 12) & 15) ^ 15, x >> 4, x & 15))
PY
}
# where a byte sequence is in a ROM (hex), plus an offset
romaddr() { python3 -c "import sys; print('%06X' % (open(sys.argv[1],'rb').read().find(bytes.fromhex(sys.argv[2])) + int(sys.argv[3])))" "$1" "$2" "$3"; }
cht() { # desc code enable, as libretro-database writes them
	printf 'cheat%d_desc = "%s"\ncheat%d_code = "%s"\ncheat%d_enable = %s\n\n' "$N" "$1" "$N" "$2" "$N" "$3"; N=$((N + 1))
}

if want 80; then
echo "== 80. cheats: the codes as the core reads them, its patches applied and taken back, .cht files, a game's file found"
OUT=$(ASAN_OPTIONS=detect_leaks=1 timeout 120 "$CHEATS_TEST" 2>&1)
expect "[ -x $CHEATS_TEST ] && echo \"\$OUT\" | grep -q ', 0 failed'" "the unit test (build/host/cheats-test): $(echo "$OUT" | tail -1) $(echo "$OUT" | grep FAIL | head -3 | tr '\n' '|')"
# the codes the next groups build, read back by the core's decoder
expect "[ \"\$($CHEATS_TEST decode md $(ggmd 000234 0000))\" = 000234:0000:00 ]" "a Mega Drive Game Genie code built here ($(ggmd 000234 0000)) is \$000234 = \$0000 to the core"
expect "[ \"\$($CHEATS_TEST decode sms $(ggsms 0013 30 03))\" = 000013:0030:03 ]" "a Master System Game Genie code built here ($(ggsms 0013 30 03)) is \$0013 = \$30 if \$03 to the core"
expect "[ \"\$($CHEATS_TEST decode md FF0001:00)\" = FF0001:0000:00 ] && [ \"\$($CHEATS_TEST decode sms 00C0-0030)\" = FF0000:0030:00 ]" "RAM codes: FF0001:00 is the byte \$00 at \$FF0001; 00C0-0030 is \$30 at \$C000"
fi

if want 81; then
echo "== 81. cheats, Mega Drive: libretro-database's folder, the cheat manager (switch, all off, all on, page), the ROM given back"
T=$(newroot t81)
$ROM "$T/root/roms/Alpha.md" ntsc >/dev/null
A=$(romaddr "$T/root/roms/Alpha.md" 323C000E 2) # move.w #\$000E,d1: the backdrop's red
DB="$T/root/cheats/Sega - Mega Drive - Genesis"
mkdir -p "$DB"
N=0
{ echo "cheats = 3"; echo; cht "Blue backdrop" "$A:0E00" false; cht "Fill in the number" "FFA3BF:??" false
  cht "Black backdrop (Game Genie)" "$(ggmd $A 0000)" false; } >"$DB/Alpha (USA).cht"
cp "$DB/Alpha (USA).cht" "$T/orig.cht"
# red; Blue on (Cross); + Black on (Down, Cross); all off (Square); all on (Triangle), R1 (to the last), Black off
P="0:0;$(CHEATMENU_AT 100);$(PRESS 180 $CROSS);$(PRESS 190 $CIRCLE);$(PRESS 200 $CIRCLE)"
P="$P;$(CHEATMENU_AT 260);$(PRESS 340 $DOWN);$(PRESS 350 $CROSS);$(PRESS 360 $CIRCLE);$(PRESS 370 $CIRCLE)"
P="$P;$(CHEATMENU_AT 430);$(PRESS 510 $SQUARE);$(PRESS 520 $CIRCLE);$(PRESS 530 $CIRCLE)"
P="$P;$(CHEATMENU_AT 590);$(PRESS 670 $TRIANGLE);$(PRESS 680 $R1);$(PRESS 690 $CROSS);$(PRESS 700 $CIRCLE);$(PRESS 710 $CIRCLE)"
rc=$(run "$T" "$P;$(QUITAT 770)" "90,175,240,410,570,750" "$T/root/roms/Alpha.md")
OWN="$T/root/cheats/MegaDrive/Alpha.cht"
expect "[ $rc = 0 ] && grep -q 'Loaded 2 cheats from .*Sega - Mega Drive - Genesis/Alpha (USA).cht (3 in the file)' $T/root/logs/boot.log" "Alpha.md finds 'Sega - Mega Drive - Genesis/Alpha (USA).cht': 2 usable cheats of 3 (FFA3BF:?? isn't a code)"
expect "$CHECK $T/dump/flip00090.ppm 960 540 red >/dev/null" "the cheats are off: red"
expect "$CHECK $T/dump/flip00240.ppm 960 540 blue >/dev/null" "Blue backdrop switched on: the ROM patched (an Action Replay code), blue"
expect "$CHECK $T/dump/flip00410.ppm 960 540 black >/dev/null" "Black backdrop (a Game Genie code) on too: the later patch of the same word wins, black"
expect "$CHECK $T/dump/flip00570.ppm 960 540 red >/dev/null" "Square (all off): the ROM's own word is back, red"
expect "$CHECK $T/dump/flip00750.ppm 960 540 blue >/dev/null" "Triangle (all on), R1 to the last cheat, Cross: Black off again, blue"
expect "[ -f '$OWN' ] && grep -q 'cheat0_enable = true' '$OWN' && grep -q 'cheat1_code = \"FFA3BF:??\"' '$OWN' && grep -q 'cheat1_enable = false' '$OWN' && grep -q 'cheat2_enable = false' '$OWN' && grep -q '^cheats = 3' '$OWN'" "each switch saved in cheats/MegaDrive/Alpha.cht: all three cheats, the unusable one as it was"
expect "cmp -s '$DB/Alpha (USA).cht' $T/orig.cht" "libretro-database's file is left as it was"
nosan "$T"
rc=$(run "$T" "0:0;$(QUITAT 120)" "90" "$T/root/roms/Alpha.md")
expect "[ $rc = 0 ] && grep -q 'Loaded 2 cheats from .*cheats/MegaDrive/Alpha.cht' $T/root/logs/boot.log && $CHECK $T/dump/flip00090.ppm 960 540 blue >/dev/null" "the next start: the game's own file, Blue backdrop on from the first frame"
nosan "$T"
fi

if want 82; then
echo "== 82. cheats, Master System and Game Gear: Game Genie ROM patches (with the byte they replace), RAM codes"
T=$(newroot t82)
$ROM "$T/root/roms/test.sms" >/dev/null
$ROM "$T/root/roms/test.gg" >/dev/null
$ROM "$T/root/roms/ramcolor.sms" cheat >/dev/null
$ROM "$T/root/roms/ramcolor.gg" cheat >/dev/null
mkdir -p "$T/root/cheats/MasterSystem" "$T/root/cheats/Sega - Game Gear"
S=$(romaddr "$T/root/roms/test.sms" 0603 1) # LD B,\$03: red
G=$(romaddr "$T/root/roms/test.gg" 060F 1) # LD B,\$0F: red
N=0; { echo "cheats = 2"; cht "Wrong reference" "$(ggsms ${S:2} 0C 55)" true; cht "Blue" "$(ggsms ${S:2} 30 03)" true; } >"$T/root/cheats/MasterSystem/test.cht"
N=0; { echo "cheats = 1"; cht "Black" "$(ggsms ${G:2} 00 0F)" true; } >"$T/root/cheats/Sega - Game Gear/test.cht"
N=0; { echo "cheats = 1"; cht "Blue (Action Replay)" "00C0-0030" true; } >"$T/root/cheats/ramcolor.cht"
for g in test.sms ramcolor.sms; do
	rc=$(run "$T" "0:0;$(QUITAT 120)" "90" "$T/root/roms/$g")
	expect "[ $rc = 0 ] && $CHECK $T/dump/flip00090.ppm 960 540 blue >/dev/null" "Master System $g: blue ($([ $g = test.sms ] && echo 'a Game Genie ROM patch; the one whose reference byte is not there does nothing' || echo 'an Action Replay RAM code at \$C000'))"
	nosan "$T"
done
N=0; { echo "cheats = 1"; cht "Black (Fusion RAM)" "C000:00" true; } >"$T/root/cheats/ramcolor.cht"
for g in test.gg ramcolor.gg; do
	rc=$(run "$T" "0:0;$(QUITAT 120)" "90" "$T/root/roms/$g")
	expect "[ $rc = 0 ] && $CHECK $T/dump/flip00090.ppm 960 540 black >/dev/null" "Game Gear $g: black ($([ $g = test.gg ] && echo "a Game Genie ROM patch, from 'Sega - Game Gear/'" || echo 'a RAM code at \$C000'))"
	nosan "$T"
done
fi

if want 83; then
echo "== 83. cheats on RAM: Mega Drive words and bytes written every frame; Sega CD (Work-RAM, 'Sega - Mega-CD - Sega CD/')"
T=$(newroot t83)
$ROM "$T/root/roms/ram.md" cheat >/dev/null
mkdir -p "$T/root/cheats/MegaDrive"
N=0; { echo "cheats = 1"; cht "Blue" "FF0000:0E00" true; } >"$T/root/cheats/MegaDrive/ram.cht"
rc=$(run "$T" "0:0;$(QUITAT 120)" "90" "$T/root/roms/ram.md")
expect "[ $rc = 0 ] && $CHECK $T/dump/flip00090.ppm 960 540 blue >/dev/null" "FF0000:0E00: the word the game reads its colour from, blue"
nosan "$T"
# a byte: libretro-database's 2-digit Action Replay codes, and a 4-digit one with a value under \$100
for code in FF0001:00 FF0001:0000; do
	N=0; { echo "cheats = 1"; cht "Black" "$code" true; } >"$T/root/cheats/MegaDrive/ram.cht"
	rc=$(run "$T" "0:0;$(QUITAT 120)" "90" "$T/root/roms/ram.md")
	expect "[ $rc = 0 ] && $CHECK $T/dump/flip00090.ppm 960 540 black >/dev/null" "$code: the byte at \$FF0001 (red \$000E -> \$0000), not its neighbour: black"
done
nosan "$T"
CDMK="python3 $PWD/tests/make_test_cd.py"
T=$(newroot t83cd)
mkdir -p "$T/root/bios" "$T/root/cheats/Sega - Mega-CD - Sega CD"
$ROM "$T/root/bios/bios_CD_U.bin" cdbioscheat >/dev/null
$CDMK "$T/root/roms/Test (USA).cue" --region U >/dev/null
N=0; { echo "cheats = 2"; cht "Blue" "FF0000:0E00" true; cht "PRG-RAM" "01A000:12" true; } >"$T/root/cheats/Sega - Mega-CD - Sega CD/Test (USA).cht"
rc=$(run "$T" "0:0;$(QUITAT 200)" "90" "$T/root/roms/Test (USA).cue")
expect "[ $rc = 0 ] && grep -q 'running \"Test (USA)\" (Sega CD)' $T/root/logs/boot.log && grep -q 'Loaded 2 cheats from .*Sega - Mega-CD - Sega CD/Test (USA).cht' $T/root/logs/boot.log" "a Sega CD disc finds its file in 'Sega - Mega-CD - Sega CD/' (a PRG-RAM code is usable there)"
expect "$CHECK $T/dump/flip00090.ppm 960 540 blue >/dev/null" "Sega CD: the Work-RAM code runs, blue"
nosan "$T"
fi

if want 84; then
echo "== 84. cheats follow their file (2.7): codes added while the game runs or the cheat manager is open apply at once"
# the game runs paced (GENPLUS_HOST_REALTIME) while this script writes the .cht as an FTP upload would (all of it at once)
T=$(newroot t84)
$ROM "$T/root/roms/live.md" cheat >/dev/null
mkdir -p "$T/root/cheats/MegaDrive"
CHT=$T/root/cheats/MegaDrive/live.cht
putcht() { cat >"$T/new.cht" && mv "$T/new.cht" "$CHT"; }
waitlog() { for i in $(seq 1 300); do grep -q "$1" "$T/root/logs/boot.log" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
( GENPLUS_HOST_REALTIME=1 MAXFLIPS=2000 run "$T" "0:0;$(QUITAT 1500)" "1480" "$T/root/roms/live.md" >"$T/rc.txt" ) &
GAME=$!
waitlog '\[emu\] frame'
expect "grep -q '\[cheats\] no .cht for live' $T/root/logs/boot.log" "the game started with no cheat file"
N=0; { echo "cheats = 1"; cht "Blue" "FF0000:0E00" true; } | putcht
waitlog 'reloaded: 1 cheats'
expect "grep -q 'reloaded: 1 cheats from live.cht' $T/root/logs/boot.log" "a cheat file that appeared while the game runs was loaded"
N=0; { echo "cheats = 2"; cht "Blue" "FF0000:0E00" true; cht "Black" "FF0000:00" false; } | putcht
waitlog 'reloaded: 2 cheats'
expect "grep -q 'reloaded: 2 cheats from live.cht' $T/root/logs/boot.log" "a cheat added to the file was loaded"
echo "this is not a cheat file" | putcht
waitlog 'has no usable cheat'
expect "grep -q 'live.cht has no usable cheat: the 2 that run stay' $T/root/logs/boot.log" "a file with no usable cheat leaves the running cheats alone"
N=0; { echo "cheats = 3"; cht "Blue" "FF0000:0E00" false; cht "Black" "FF0000:00" true; cht "Extra" "FF0100:0001" true; } | putcht
waitlog 'reloaded: 3 cheats'
expect "grep -q 'reloaded: 3 cheats from live.cht' $T/root/logs/boot.log" "and the file written next is loaded too"
wait $GAME
rc=$(cat "$T/rc.txt")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "$CHECK $T/dump/flip01480.ppm 960 540 black >/dev/null" "the last file's cheats run: Blue off, Black on (black)"
expect "grep -q 'cheat2_desc = \"Extra\"' $CHT && ! grep -q 'saved' <(grep '\[cheats\]' $T/root/logs/boot.log)" "the file is left as written (closing the game writes nothing over it)"
nosan "$T"
# the cheat manager open, empty: libretro-database's file copied meanwhile shows up in it; Cross switches its cheat on
T=$(newroot t84b)
$ROM "$T/root/roms/live2.md" cheat >/dev/null
mkdir -p "$T/root/cheats/Sega - Mega Drive - Genesis"
P="0:0;$(CHEATMENU_AT 60);$(PRESS 700 $CROSS);$(PRESS 710 $CIRCLE);$(PRESS 720 $CIRCLE)"
( GENPLUS_HOST_REALTIME=1 MAXFLIPS=2000 run "$T" "$P;$(QUITAT 800)" "780" "$T/root/roms/live2.md" >"$T/rc.txt" ) &
GAME=$!
waitlog '\[emu\] frame'
sleep 3 # the cheat manager is open (frame 130 on), showing "No cheats found"
N=0; { echo "cheats = 1"; cht "Blue" "FF0000:0E00" false; } >"$T/new.cht"
mv "$T/new.cht" "$T/root/cheats/Sega - Mega Drive - Genesis/Live2 (USA).cht"
wait $GAME
rc=$(cat "$T/rc.txt")
expect "[ $rc = 0 ] && grep -q 'reloaded: 1 cheats from Live2 (USA).cht' $T/root/logs/boot.log" "the file copied while the cheat manager is open is listed in it"
expect "$CHECK $T/dump/flip00780.ppm 960 540 blue >/dev/null && grep -q 'cheat0_enable = true' '$T/root/cheats/MegaDrive/live2.cht'" "Cross switched its cheat on (blue), saved in cheats/MegaDrive/live2.cht"
nosan "$T"
fi

if want 85; then
echo "== 85. the cheat manager's empty screen in every language"
for L in 1 2 3 4; do
	T=$(newroot t85_$L)
	echo "language=$L" >>"$T/root/genplus-ps5.ini"
	$ROM "$T/root/roms/test.md" ntsc >/dev/null
	rc=$(GENPLUS_HOST_I18N_MISSING=$T/missing.txt run "$T" "0:0;$(CHEATMENU_AT 60);$(PRESS 150 $CIRCLE);$(PRESS 160 $CIRCLE);$(QUITAT 180)" "140" "$T/root/roms/test.md")
	expect "[ $rc = 0 ] && [ -f $T/dump/flip00140.ppm ] && ! grep -q -e 'cheat' -e 'Cheat' -e '\.cht' -e 'Back' -e 'FTP' $T/missing.txt" "language $L: the empty cheat manager is translated ($(grep -i -e cheat -e cht -e Back $T/missing.txt | head -3 | tr '\n' '|'))"
	nosan "$T"
done
fi

# ---------------- 2.1: borders around the picture and more covers (Snes9x PS5 2.7's, pull request #3 by thatboialex) ----------------
BORDERCHK="python3 $PWD/tests/check_border.py"
mkborder() { # file [x0 y0 x1 y1]: a 1920x1080 blue border with a clear window (261,21 1398x1038 unless given)
	mkdir -p "$(dirname "$1")"
	python3 -c "
import sys
from PIL import Image
im = Image.new('RGBA', (1920, 1080), (0, 0, 255, 255))
im.paste((0, 0, 0, 0), tuple(int(v) for v in sys.argv[2:6]))
im.save(sys.argv[1])" "$1" ${2:-261} ${3:-21} ${4:-1659} ${5:-1059}
}

if want 60; then
echo "== 60. unit tests: the cover and border matcher, the border pack, the border in the renderer (CPU and GPU surface), the helper's two download lanes"
for t in titlematch-test border-pack-test border-render-test download-worker-test; do
	if [ ! -x build/host/$t ]; then bad "$t is built (make unit-tests)"; continue; fi
	ASAN_OPTIONS=detect_leaks=1 timeout 600 build/host/$t >$WORK/$t.txt 2>&1
	rc=$?
	expect "[ $rc = 0 ] && ! grep -q 'runtime error\|AddressSanitizer\|^FAIL' $WORK/$t.txt" "$t: $(grep -E 'checks,|all checks' $WORK/$t.txt | tail -1)"
done
fi

if want 61; then
echo "== 61. border packs: a game finds its border (own file name, the cover matcher), its system's default.png, default.png, the built-in frame"
T=$(newroot t61)
B=$T/root/borders
$ROM "$T/root/roms/test.md" ntsc >/dev/null
$ROM "$T/root/roms/Langrisser 2 English.md" ntsc >/dev/null
$ROM "$T/root/roms/Unknown Game.md" ntsc >/dev/null
bordercase() { run "$T" "0:0;$(QUITAT 60)" "" "$T/root/roms/$1"; }
rc=$(bordercase "test.md")
expect "[ $rc = 0 ] && ! grep -q '\[border\]' $T/root/logs/boot.log" "Border Off (the default): the pack is not read, nothing is logged"
echo "border=1" >>"$T/root/genplus-ps5.ini"
echo "border_download=0" >>"$T/root/genplus-ps5.ini"
mkborder "$B/MegaDrive/Langrisser II (Japan).png" 300 60 1600 1020
rc=$(bordercase "Langrisser 2 English.md")
expect "[ $rc = 0 ] && grep -q '\[border\] Langrisser 2 English -> MegaDrive/Langrisser II (Japan).png (matched): window' $T/root/logs/boot.log" "a translation, named its own way, finds the Japanese game's border through the cover matcher"
rc=$(bordercase "Unknown Game.md")
expect "[ $rc = 0 ] && grep -q '\[border\] Unknown Game -> built-in (built-in): window' $T/root/logs/boot.log" "a game with no border and no default.png: the frame built into the app"
mkborder "$B/default.png" 200 40 1700 1040
rc=$(bordercase "Unknown Game.md")
expect "grep -q '\[border\] Unknown Game -> default.png (default.png)' $T/root/logs/boot.log" "then default.png"
mkborder "$B/MegaDrive/default.png"
rc=$(bordercase "Unknown Game.md")
expect "grep -q '\[border\] Unknown Game -> MegaDrive/default.png (system default.png)' $T/root/logs/boot.log" "then the system's own MegaDrive/default.png"
mkborder "$B/MegaDrive/test.png"
echo "window=100,100,900,600" >"$B/MegaDrive/test.ini"
rc=$(bordercase "test.md")
expect "grep -q '\[border\] test -> MegaDrive/test.png (own file): window' $T/root/logs/boot.log" "MegaDrive/<ROM name>.png wins, and its .ini gives the window"
echo "off=1" >"$B/MegaDrive/test.ini"
rc=$(bordercase "test.md")
expect "grep -q '\[border\] test: none, an .ini says off=1' $T/root/logs/boot.log" "off=1 in the .ini: no border for that game"
rm "$B/MegaDrive/test.ini"
echo "garbage" >"$B/MegaDrive/test.png"
rc=$(bordercase "test.md")
expect "grep -q '\[border\] MegaDrive/test.png: not a PNG' $T/root/logs/boot.log && grep -q 'test -> MegaDrive/default.png' $T/root/logs/boot.log" "a broken file is logged and skipped"
sed -i 's/^border=1/border=2/' "$T/root/genplus-ps5.ini"
rc=$(bordercase "Langrisser 2 English.md")
expect "grep -q '\[border\] Langrisser 2 English -> MegaDrive/default.png (system default.png)' $T/root/logs/boot.log" "Default frame: the game's own border is not looked for"
nosan "$T"
# a Game Gear game in a zip on a share: the border is picked before the game runs, by the system the loader found inside the zip
# (the zip is not opened again for it), and nothing is read from the share for it while the game runs
T=$(newroot t61z)
mkdir -p "$T/share/misc"
$ROM "$T/gg.gg" >/dev/null
python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1],'w'); z.write(sys.argv[2],'Pocket.gg'); z.close()" "$T/share/misc/pocket.zip" "$T/gg.gg"
mkborder "$T/root/borders/GameGear/pocket.png" 274 0 1646 1080
mkborder "$T/root/borders/MegaDrive/pocket.png"
printf 'border=1\nborder_download=0\n' >>"$T/root/genplus-ps5.ini"
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
rc=$(GENPLUS_HOST_REALTIME=1 MAXFLIPS=900 GENPLUS_HOST_NETFS_DIR="$T/share" run "$T" "0:0;150:$CROSS;152:0;$(QUITAT 300)" "")
expect "[ $rc = 0 ] && grep -q 'loading /smb/10.0.0.5/games/misc/pocket.zip' $T/root/logs/boot.log && grep -q '\[border\] pocket -> GameGear/pocket.png (own file): window [0-9]*,0 [0-9]*x1080' $T/root/logs/boot.log" "a Game Gear zip on a share gets its GameGear border (the loader's system), not the Mega Drive one"
expect "! grep -q 'in realtime' $T/root/logs/boot.log && [ \$(grep -n 'border\] pocket ->' $T/root/logs/boot.log | head -1 | cut -d: -f1) -lt \$(grep -n 'running \"' $T/root/logs/boot.log | head -1 | cut -d: -f1) ]" "it is picked before the game runs, and nothing waits for the share while it runs"
nosan "$T"
fi

if want 62; then
echo "== 62. borders drawn: Off changes nothing; Auto draws the game's border (CPU and GPU, with shaders); the pause menu darkens both; the window follows the picture; the Border row"
# a. Off: black around the picture whatever is in borders/
T=$(newroot t62a)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
mkborder "$T/root/borders/MegaDrive/test.png"
rc=$(run "$T" "0:0;$(QUITAT 120)" "90" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && $RECT $T/dump/flip00090.ppm $T/root/logs/boot.log red >/dev/null && ! grep -q '\[border\]' $T/root/logs/boot.log" "Border Off: the picture with black around it, as before"
# b. Auto: the border around the picture, the picture fills the window fitted to its shape; the pause menu darkens both; Resume
T=$(newroot t62b)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
mkborder "$T/root/borders/MegaDrive/test.png"
echo "border=1" >>"$T/root/genplus-ps5.ini"
rc=$(run "$T" "0:0;60:$L3R3;65:0;100:$CIRCLE;102:0;$(QUITAT 160)" "50,90,130" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && $BORDERCHK $T/dump/flip00050.ppm $T/root/logs/boot.log blue red fills >/dev/null" "Border Auto: the game's border around the window, the picture filling it"
expect "$CHECK $T/dump/flip00090.ppm 100 540 0 0 63 >/dev/null && $CHECK $T/dump/flip00090.ppm 300 540 63 0 0 12 >/dev/null" "the pause menu darkens the border and the picture together"
expect "$BORDERCHK $T/dump/flip00130.ppm $T/root/logs/boot.log blue red fills >/dev/null" "Resume: the border and the picture are back"
nosan "$T"
# c. Default frame: the built-in one, even with a border of the game's own
def_frame=$(python3 -c "
from PIL import Image
r, g, b, a = Image.open('frontend/assets/borders/default.png').convert('RGBA').getpixel((30, 540))
print(r * a // 255, g * a // 255, b * a // 255)")
T=$(newroot t62c)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
mkborder "$T/root/borders/MegaDrive/test.png"
echo "border=2" >>"$T/root/genplus-ps5.ini"
rc=$(run "$T" "0:0;$(QUITAT 120)" "90" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && $CHECK $T/dump/flip00090.ppm 30 540 $def_frame 3 >/dev/null && $CHECK $T/dump/flip00090.ppm 960 540 red >/dev/null && grep -q 'built-in (built-in)' $T/root/logs/boot.log" "border=2: the built-in frame around the picture"
# d. the window follows the Screen size: Integer scale cuts it to 4x the 224-line picture (896 lines); Stretch makes it 16:9 (the
# border is picked for the core's 192-line size at load and refitted to the first frame's 224 lines)
for mode in "1 896" "2 963"; do
	set -- $mode
	T=$(newroot t62d$1)
	$ROM "$T/root/roms/test.md" ntsc >/dev/null
	mkborder "$T/root/borders/MegaDrive/test.png"
	printf 'border=1\nscale=%s\n' "$1" >>"$T/root/genplus-ps5.ini"
	rc=$(run "$T" "0:0;$(QUITAT 120)" "90" "$T/root/roms/test.md")
	expect "[ $rc = 0 ] && grep -Eq '\[border\] (test -> MegaDrive/test.png \(own file\)|MegaDrive/test.png refitted): window [0-9]*,[0-9]* [0-9]*x$2' $T/root/logs/boot.log && $BORDERCHK $T/dump/flip00090.ppm $T/root/logs/boot.log blue red fills >/dev/null" "scale=$1: the window is $2 lines tall and the picture fills it"
done
# e. a Game Gear game in a Game Gear bezel (The Bezel Project's window: 274,0 1372x1080): the handheld's 4:3 picture, full height
T=$(newroot t62e)
$ROM "$T/root/roms/handheld.gg" >/dev/null
mkborder "$T/root/borders/GameGear/handheld.png" 274 0 1646 1080
echo "border=1" >>"$T/root/genplus-ps5.ini"
rc=$(run "$T" "0:0;$(QUITAT 120)" "90" "$T/root/roms/handheld.gg")
expect "[ $rc = 0 ] && grep -q '\[border\] handheld -> GameGear/handheld.png (own file): window [0-9]*,0 [0-9]*x1080' $T/root/logs/boot.log && $BORDERCHK $T/dump/flip00090.ppm $T/root/logs/boot.log blue red fills >/dev/null" "Game Gear: its border's window is the handheld's screen, fitted to its picture, which fills it"
nosan "$T"
# f. the Border row: Settings from the shelf (Shader, Screen size, Aspect ratio, Border: Down three times), Right twice
T=$(newroot t62f)
$ROM "$T/root/roms/Alpha (USA).md" ntsc >/dev/null
rc=$(run "$T" "0:0;30:$TRIANGLE;32:0;40:$DOWN;42:0;46:$DOWN;48:0;52:$DOWN;54:0;58:$RIGHT;60:0;64:$RIGHT;66:0;72:$CIRCLE;74:0;82:$OPTIONS;84:0;92:$CROSS;94:0" "")
expect "[ $rc = 0 ] && grep -q '^border=2$' $T/root/genplus-ps5.ini" "Settings > Border: Right twice is Default frame (border=2)"
# g. the pause menu's Settings (Down five times), Border (Down three), Right: Auto at once, without restarting the game
T=$(newroot t62g)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
mkborder "$T/root/borders/MegaDrive/test.png"
pad="0:0;60:$L3R3;65:0"; t=80
for i in 1 2 3 4 5; do pad="$pad;$t:$DOWN;$((t + 2)):0"; t=$((t + 6)); done
pad="$pad;$t:$CROSS;$((t + 2)):0"; t=$((t + 10))
for i in 1 2 3; do pad="$pad;$t:$DOWN;$((t + 2)):0"; t=$((t + 6)); done
pad="$pad;$t:$RIGHT;$((t + 1)):0;$((t + 10)):$CIRCLE;$((t + 12)):0;$((t + 20)):$CIRCLE;$((t + 22)):0"
rc=$(run "$T" "$pad;$(QUITAT $((t + 90)))" "50,$((t + 70))" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && grep -q '^border=1$' $T/root/genplus-ps5.ini" "the pause menu's Settings > Border sets Auto (border=1, saved)"
expect "$CHECK $T/dump/flip00050.ppm 100 540 0 0 0 >/dev/null" "before: no border"
expect "$BORDERCHK $T/dump/flip$(printf %05d $((t + 70))).ppm $T/root/logs/boot.log blue red fills >/dev/null" "after Resume the game's border is there, without restarting the game"
nosan "$T"
# i. the pause menu's Settings, Screen size (Down once), Right: Integer scale. The picture's new shape refits the border chosen
# at load (from its decoded copy): it is not picked again
T=$(newroot t62i)
$ROM "$T/root/roms/test.md" ntsc >/dev/null
mkborder "$T/root/borders/MegaDrive/test.png"
echo "border=1" >>"$T/root/genplus-ps5.ini"
pad="0:0;60:$L3R3;65:0"; t=80
for i in 1 2 3 4 5; do pad="$pad;$t:$DOWN;$((t + 2)):0"; t=$((t + 6)); done
pad="$pad;$t:$CROSS;$((t + 2)):0;$((t + 10)):$DOWN;$((t + 12)):0"; t=$((t + 20))
pad="$pad;$t:$RIGHT;$((t + 1)):0;$((t + 10)):$CIRCLE;$((t + 12)):0;$((t + 20)):$CIRCLE;$((t + 22)):0"
rc=$(run "$T" "$pad;$(QUITAT $((t + 90)))" "50,$((t + 70))" "$T/root/roms/test.md")
expect "[ $rc = 0 ] && grep -q '^scale=1$' $T/root/genplus-ps5.ini" "the pause menu's Settings > Screen size sets Integer scale"
expect "[ \$(grep -c '\[border\] test -> MegaDrive/test.png (own file)' $T/root/logs/boot.log) = 1 ] && grep -q '\[border\] MegaDrive/test.png refitted: window [0-9]*,[0-9]* [0-9]*x896' $T/root/logs/boot.log" "the border is picked once; the new shape refits it (4x the 224 lines)"
expect "$BORDERCHK $T/dump/flip$(printf %05d $((t + 70))).ppm $T/root/logs/boot.log blue red fills >/dev/null" "after Resume the picture fills the refitted window"
nosan "$T"
# h. the GPU renderer (lavapipe): the border on the surface over the GPU's picture, plain and through a GPU shader
for s in 0 1; do
	T=$(newroot t62h$s)
	$ROM "$T/root/roms/test.md" ntsc >/dev/null
	mkborder "$T/root/borders/MegaDrive/test.png"
	printf 'border=1\nshader=%s\n' $s >>"$T/root/genplus-ps5.ini"
	rc=$(GENPLUS_HOST_GPU_SYNC_BUILD=1 GENPLUS_HOST_GPU=1 run "$T" "0:0;$(QUITAT 60)" "50,66" "$T/root/roms/test.md")
	if [ $s = 0 ]; then pic=red; else pic=other; fi
	expect "[ $rc = 0 ] && grep -q 'renderer: GPU' $T/root/logs/boot.log && $CHECK $T/dump/flip00050.ppm 100 540 blue >/dev/null && python3 -c \"import sys; d=open('$T/dump/flip00050.ppm','rb').read().split(b'\\n',3); w=int(d[1].split()[0]); p=d[3]; sys.exit(0 if max(p[(y*w+960)*3] for y in range(440,640)) > 60 else 1)\"" "GPU, shader=$s: the border around the window, the game in it"
	expect "$CHECK $T/dump/flip00066.ppm 100 540 0 0 63 >/dev/null" "GPU, shader=$s: the pause menu darkens the border too"
	nosan "$T"
done
fi

if want 63; then
echo "== 63. border downloads: Auto asks the helper for the game's border, it lands in borders/<system> and goes up while the game runs; Settings, Download all borders"
stop_helpers
T=$(newroot t63a)
SRV=$T/srv/GameBezels
mkborder "$SRV/Sonic The Hedgehog 2 (World).png"
$ROM "$T/root/roms/Sonic the Hedgehog 2.md" ntsc >/dev/null
echo "border=1" >>"$T/root/genplus-ps5.ini"
PORT=18163
SERVER_DELAY=1 python3 tests/https_server.py "$T/srv" $PORT >/dev/null 2>&1 &
SRVPID=$!
GENPLUS_PS5_ROOT=$T/root ASAN_OPTIONS=detect_leaks=0 timeout 120 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/root/logs/helper.log" "listening" || sleep 1
export GENPLUS_BORDER_URL="http://127.0.0.1:$PORT/GameBezels/"
rc=$(OFFLINE= GENPLUS_HOST_REALTIME=1 run "$T" "0:0;$(QUITAT 400)" "30,380" "$T/root/roms/Sonic the Hedgehog 2.md")
unset GENPLUS_BORDER_URL
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '\[border\] Sonic the Hedgehog 2: asking the helper for MegaDrive/Sonic The Hedgehog 2 (World).png' $T/root/logs/boot.log" "Auto, no border in the pack: the app asks the helper for The Bezel Project's name for the game"
expect "grep -q '\[borders\] MegaDrive/Sonic The Hedgehog 2 (World).png -> 200' $T/root/logs/helper.log && cmp -s '$T/root/borders/MegaDrive/Sonic The Hedgehog 2 (World).png' '$SRV/Sonic The Hedgehog 2 (World).png'" "the helper downloaded it into borders/MegaDrive/"
expect "$CHECK $T/dump/flip00030.ppm 30 540 $def_frame 3 >/dev/null" "the game started with the built-in frame"
expect "grep -q '\[border\] Sonic the Hedgehog 2 -> MegaDrive/Sonic The Hedgehog 2 (World).png (downloaded): window' $T/root/logs/boot.log && $BORDERCHK $T/dump/flip00380.ppm $T/root/logs/boot.log blue red fills >/dev/null" "the downloaded border goes up while the game runs"
expect "grep -q '^0 1 idle' $T/root/borders/progress.txt" "borders/progress.txt: nothing left, 1 fetched"
nosan "$T"
kill $SRVPID 2>/dev/null
stop_helpers
rm -f "$T/root/borders/priority.txt"
rc=$(run "$T" "0:0;$(QUITAT 60)" "" "$T/root/roms/Sonic the Hedgehog 2.md")
expect "[ $rc = 0 ] && grep -q '\[border\] Sonic the Hedgehog 2 -> MegaDrive/Sonic The Hedgehog 2 (World).png (matched)' $T/root/logs/boot.log && ! grep -q 'asking the helper' $T/root/logs/boot.log" "the next start finds the downloaded border in the pack and asks for nothing"
# border_download=0: Auto never asks
T=$(newroot t63b)
$ROM "$T/root/roms/Sonic the Hedgehog 2.md" ntsc >/dev/null
printf 'border=1\nborder_download=0\n' >>"$T/root/genplus-ps5.ini"
rc=$(run "$T" "0:0;$(QUITAT 60)" "" "$T/root/roms/Sonic the Hedgehog 2.md")
expect "[ $rc = 0 ] && ! grep -q 'asking the helper' $T/root/logs/boot.log && [ ! -e $T/root/borders/priority.txt ] && grep -q '^border_download=0$' $T/root/genplus-ps5.ini" "border_download=0: the helper is not asked (and the setting is kept)"
# Settings > Download all borders (LIBRARY, below Download covers: Up seven times from the top wraps to it), in Spanish: two ROMs of one
# game, a Japanese translation, a Game Gear game and one without a border
T=$(newroot t63c)
$ROM "$T/root/roms/Sonic The Hedgehog 2 (World).md" ntsc >/dev/null
$ROM "$T/root/roms/Sonic the Hedgehog 2.md" ntsc >/dev/null
$ROM "$T/root/roms/Langrisser 2 English.md" ntsc >/dev/null
$ROM "$T/root/roms/Sonic The Hedgehog (Japan, USA) (En).gg" >/dev/null
$ROM "$T/root/roms/Totally Unknown Homebrew.md" ntsc >/dev/null
echo "language=1" >>"$T/root/genplus-ps5.ini"
pad="0:0;30:$TRIANGLE;32:0"; t=40
for i in 1 2 3 4 5 6 7; do pad="$pad;$t:$UP;$((t + 2)):0"; t=$((t + 6)); done
export GENPLUS_BORDER_URL="http://127.0.0.1:$PORT/GameBezels/"
rc=$(GENPLUS_HOST_I18N_MISSING=$T/missing.txt run "$T" "$pad;$t:$CROSS;$((t + 2)):0;$((t + 30)):$CIRCLE;$((t + 32)):0;$((t + 40)):$OPTIONS;$((t + 42)):0;$((t + 50)):$CROSS;$((t + 52)):0" "")
unset GENPLUS_BORDER_URL
expect "[ $rc = 0 ] && grep -q '\[border\] user asked for the borders of the library: 3 to download' $T/root/logs/boot.log" "Download all borders: three pictures (two ROMs share one, the translation and the Game Gear game have their own, the homebrew none)"
expect "[ \$(wc -l <$T/root/borders/wanted.txt) = 3 ] && grep -q '^MegaDrive/Sonic The Hedgehog 2 (World).png.http://127.0.0.1:$PORT/GameBezels/Sonic%20The%20Hedgehog%202%20%28World%29.png\$' $T/root/borders/wanted.txt && grep -q '^MegaDrive/Langrisser II (Japan).png' $T/root/borders/wanted.txt && grep -q '^GameGear/Sonic The Hedgehog (Japan, USA) (En).png' $T/root/borders/wanted.txt" "borders/wanted.txt lists them for the helper, each in its system's folder"
expect "! grep -q 'border\|Border\|queued\|fetch' $T/missing.txt" "the rows and their values are translated ($(head -c 200 $T/missing.txt | tr '\n' '|'))"
nosan "$T"
fi

if want 64; then
echo "== 64. more covers: renamed and translated ROMs, a dump No-Intro lacks (its CRC), a ROM header's title, an online database's art"
stop_helpers # the app downloads them itself here (no helper: as in group 10)
T=$(newroot t64)
MDREPO=Sega_-_Mega_Drive_-_Genesis; GGREPO=Sega_-_Game_Gear
mkdir -p "$T/srv/$MDREPO/Named_Boxarts" "$T/srv/$GGREPO/Named_Boxarts" "$T/srv/db"
python3 - "$T/srv" <<'PY'
import sys
from PIL import Image
srv = sys.argv[1]
for name, colour in (("Langrisser II (Japan)", (255, 0, 0)), ("Advanced Busterhawk Gleylancer (Japan)", (0, 255, 0)), ("Bare Knuckle II - Shitou e no Requiem ~ Streets of Rage II (Japan, Europe)", (0, 0, 255)), ("Bare Knuckle II - Shitou e no Requiem ~ Streets of Rage II (Japan, Europe) (En,Ja)", (0, 0, 255))):
    Image.new('RGB', (512, 357), colour).save('%s/Sega_-_Mega_Drive_-_Genesis/Named_Boxarts/%s.png' % (srv, name))
Image.new('RGB', (512, 357), (255, 255, 0)).save(srv + '/Sega_-_Game_Gear/Named_Boxarts/Phantasy Star Gaiden (Japan).png')
Image.new('RGB', (512, 357), (255, 0, 255)).save(srv + '/db/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA', format='PNG')
PY
mkdir -p "$T/root/roms"
$ROM "$T/root/roms/Langrisser 2 (English).md" ntsc >/dev/null
$ROM "$T/root/roms/glancer-eng.md" ntsc >/dev/null
python3 tests/forge_crc.py "$T/root/roms/glancer-eng.md" $(grep -P "^md\t[0-9A-F]{8}\tAdvanced Busterhawk Gley Lancer \(Japan\)\[tr en" frontend/data/extra-crc.tsv | head -1 | cut -f2) >/dev/null
$ROM "$T/root/roms/psg.gg" >/dev/null
python3 tests/forge_crc.py "$T/root/roms/psg.gg" 1667748B >/dev/null
# a ROM whose file name and CRC say nothing: the title in its header (domestic, Japan)
$ROM "$T/root/roms/mystery.md" ntsc >/dev/null
python3 - "$T/root/roms/mystery.md" <<'PY'
import sys
p = sys.argv[1]; d = bytearray(open(p, 'rb').read())
def put(at, s): d[at:at + len(s)] = s.encode()
put(0x100, 'SEGA MEGA DRIVE '); put(0x120, 'BARE KNUCKLE II'.ljust(48)); put(0x150, 'STREETS OF RAGE II'.ljust(48)); put(0x1F0, 'J  ')
open(p, 'wb').write(d)
PY
$ROM "$T/root/roms/My Homebrew Thing.md" ntsc >/dev/null
printf '# test table\nmd\tMy Homebrew Thing\thAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\t00000000\n' >"$T/dbart.tsv"
PORT=18164
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
sleep 1
rc=$(OFFLINE= COVER_URL="http://127.0.0.1:$PORT/\${repo}/Named_Boxarts/\${name}.png" GENPLUS_HOST_DBART=$T/dbart.tsv \
	GENPLUS_DBART_HASHEOUS="http://127.0.0.1:$PORT/db/" GENPLUS_HOST_REALTIME=1 run "$T" "0:0;300:$OPTIONS;302:0;310:$CROSS;312:0" "")
kill $SRVPID 2>/dev/null
C=$T/root/covers
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "cmp -s '$C/MegaDrive/Langrisser II (Japan).png' '$T/srv/$MDREPO/Named_Boxarts/Langrisser II (Japan).png'" "a translation named its own way gets the Japanese game's cover"
expect "grep -q 'glancer-eng.md \[md\] -> \"\" (dump: Advanced Busterhawk Gley Lancer' $T/root/logs/boot.log && [ -f '$C/MegaDrive/Advanced Busterhawk Gleylancer (Japan).png' ]" "a dump No-Intro lacks: the extra table names its CRC, and the cover follows"
expect "[ -f '$C/GameGear/Phantasy Star Gaiden (Japan).png' ]" "a Game Gear translation by its CRC: the Game Gear cover"
expect "grep -q 'mystery.md \[md\] -> \"\" (header: BARE KNUCKLE II)' $T/root/logs/boot.log && ls '$C/MegaDrive/Bare Knuckle II - Shitou e no Requiem ~ Streets of Rage II (Japan, Europe)'*.png >/dev/null 2>&1" "a ROM nothing names: its header's title finds the cover"
expect "grep -q 'a database has art for My Homebrew Thing' $T/root/logs/boot.log && cmp -s '$C/MegaDrive/My Homebrew Thing.png' '$T/srv/db/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'" "a game libretro-thumbnails lacks: the online database's art, after libretro's 404"
nosan "$T"
fi

echo
echo "passed $PASS, failed $FAIL  (work dir $WORK)"
[ $FAIL = 0 ]
