# mse-replay — the muxer, through a real MSE decoder

`http_server.cpp` is not in the host test build, and the camera cannot be
restarted on demand (a warm restart is the documented hardlock trigger). That
left the /ws/video muxer covered by unit tests and a cross-compile, and by
nothing that had ever *decoded* its output.

This closes that, without touching the camera's running daemon: it takes real
frames off the live feed, re-muxes them through the **shipping** code
(`fmp4::init_segment`, `fmp4::Timeline`, `fmp4::prft`, `fmp4::fragment`), and
plays the result in headless Chrome through Media Source Extensions - the same
path `preview.js` uses, including its `readPrft()`.

## Running it

```
# 1. capture real frames from the camera (read-only, one viewer, a few seconds)
#    tools/mse-drift.ps1 shows how to open /ws/video; save the text init, the
#    ftyp/moov segment as init.bin and each moof+mdat as raw/f0000.bin ...

# 2. re-mux through the shipping muxer
g++ -std=c++17 -O1 -fno-exceptions -fno-rtti -Isrc \
    -o remux tools/mse-replay/remux.cpp src/app/http/fmp4.cpp src/app/rtsp/h264_nal.cpp
./remux <dir>                 # straight through
./remux <dir> 60 100          # and again with fragments 60..99 dropped:
                              # a 2 s stall the timeline has to absorb

# 3. serve and decode (fetch() needs an origin, file:// will not do)
node tools/mse-replay/serve.js <dir>
chrome --headless=new --disable-gpu --autoplay-policy=no-user-gesture-required \
       --virtual-time-budget=25000 --dump-dom http://127.0.0.1:8731/play.html
```

## What it proved, 2026-09-23, Chrome 153

Real T40NN MAIN frames (`avc1.640033`, 1920x1080), re-muxed by the AP15 code:

```
                         straight        with a 2 s stall
fragments                150             110
prft stripped from       150             110
buffered ranges          1               1
buffered                 0.000 .. 7.497  0.000 .. 5.498
videoWidth x Height      1920 x 1080     1920 x 1080
readyState               4               4
error                    none            none
```

* 150 frames at 20 fps is 7.5 s, and the range ends at 7.497 - the derived
  timeline is correctly scaled, not merely monotonic.
* **One** buffered range in both runs. The dropped run is the property that
  mattered: `Timeline` absorbs a long stall into its skew instead of putting a
  hole in the timeline, and a hole is somewhere the playhead stops. The browser
  confirms there is none.
* `prft` was recognised and stripped by an exact copy of upstream's
  `readPrft()` on every fragment, and the NTP timestamp round-tripped to the
  second and the quarter-second it was written with.

`totalVideoFrames` reads 2 in both runs. That is a headless artefact - with no
compositor and virtual time there is nothing to present to. Decoding
demonstrably happened: `videoWidth` is only set once a frame has been decoded,
`readyState` is 4, and `currentTime` advanced. It is **not** a throughput
measurement and is not quoted as one.

## What it does not prove

The camera's own serving path: sockets, backpressure, drop-until-key, the
demand handle. Those still need the daemon restarted and remain
`PENDING_PHYSICAL`. What is settled is that the bytes the muxer produces are
bytes a browser accepts.
