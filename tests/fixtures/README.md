# AV1 entropy feature fixtures

These AVIF files are independently generated regression inputs for Part 3 entropy state. They are consumed by `tests/features.sh`; the decoder does not depend on the tools used to create them.

`delta-q-lf.avif` was generated with the locally installed libaom tools:

```sh
ffmpeg -hide_banner -loglevel error \
  -f lavfi -i 'testsrc2=size=128x128:rate=1' -frames:v 1 \
  -pix_fmt yuv420p -f yuv4mpegpipe -y delta-q-lf.y4m
aomenc --obu --usage=2 --limit=1 --cpu-used=9 --end-usage=q \
  --cq-level=30 --enable-tpl-model=1 --deltaq-mode=6 \
  --deltaq-strength=100 --delta-lf-mode=1 \
  -o delta-q-lf.obu delta-q-lf.y4m
ffmpeg -hide_banner -loglevel error -i delta-q-lf.obu \
  -c copy -y delta-q-lf.avif
```

`segmentation.avif` uses variance AQ to emit ALT_Q segmentation. Palette and intrabc are disabled so the stream isolates Part 3 syntax:

```sh
ffmpeg -hide_banner -loglevel error \
  -f lavfi -i 'testsrc2=size=64x64:rate=1' -frames:v 1 \
  -pix_fmt yuv420p -f yuv4mpegpipe -y segmentation.y4m
aomenc --obu --usage=0 --limit=1 --cpu-used=5 --end-usage=q \
  --cq-level=30 --aq-mode=1 --deltaq-mode=0 --delta-lf-mode=0 \
  --enable-palette=0 --enable-intrabc=0 \
  -o segmentation.obu segmentation.y4m
ffmpeg -hide_banner -loglevel error -i segmentation.obu \
  -c copy -y segmentation.avif
```
