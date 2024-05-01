# cxadc_vhs_server

A terrible HTTP server made for capturing VHS with two cxadc cards and [cxadc-clock-generator-audio-adc](https://gitlab.com/wolfre/cxadc-clock-generator-audio-adc) or [cxadc-clockgen-mod](https://github.com/namazso/cxadc-clockgen-mod/).

## Usage

`cxadc_vhs_server version|<port>|unix:<socket>`

> ⚠️ Do not expose the server to the public internet. It is not intended to be secure. 

Endpoints provided:

- GET `/`: Hello world.
- GET `/version`: Version.
- GET `/start`: Start a capture. Returns a JSON with stats. Parameters:
  - `cxadc<number>`: Capture `/dev/cxadc<number>` 
  - `lname=<device name>`: Use `<device name>` ALSA device for capture. Defaults to `hw:CARD=CXADCADCClockGe`
  - `lformat=<format>`: Linear capture format. Defaults to device default.
  - `lrate=<rate>`: Linear capture sample rate. Defaults to device default.
    - `lchannels=<channels>`: Linear capture channels. Defaults to device default.
- GET `/cxadc`: Stream the data being captured from a CX card. Parameters:
  - `<number>`: Access the `<number>`th **captured** card (so if you capture `cxadc1` only, you can access it as 0, **not** 1)
- GET `/linear`: Stream the data being captured from the ALSA device.
- GET `/stats`: Capture statistics.
- GET `/stop`: Stop the current capture. Reports back how many overflows happened.

For more details such as returned JSON format test the endpoints or check the source code.

## Examples

### Remote capture

Start the server on the capture machine:

```text
$ cxadc_vhs_server 8080
```

Then queue up the download of the streams:

```text
$ aria2c -Z \
    http://192.168.1.1:8080/linear \
    http://192.168.1.1:8080/cxadc?0 \
    http://192.168.1.1:8080/cxadc?1
```

Start the capture:

```text
$ curl http://192.168.1.1:8080/start?cxadc0&cxadc1
```

Once you're done, you just need to stop it:

```text
$ curl http://192.168.1.1:8080/stop
```

### Local capture

The script `local-capture.sh` is included in the repository to aid with local captures. It runs the sever on a UNIX socket, which is the same thing as used for piping command outputs. The benefit of using the server is the sample drop resilient buffering and better starting point synchronization.

#### Dependencies

- bash
- curl
- jq
- cxadc_vhs_server
- ffmpeg (optional) with libsoxr (optional)

You can install the first three from most distros' default repositories: 

**RHEL / Fedora**

```text
yum install bash curl jq
```

**Debian / Ubuntu**

```text
apt install bash curl jq
```

The `cxadc_vhs_server` binary can be obtained from releases, or compiled from sources. The binary releases support glibc 2.17 and later.

A static ffmpeg build with libsoxr can be obtained from https://johnvansickle.com/ffmpeg/. If placed next to the script, it will be used instead of system ffmpeg.

#### Usage

```text
Usage: local-capture.sh [options] <basepath>
        --video=          Number of CX card to use for video capture (unset=disabled)
        --hifi=           Number of CX card to use for hifi capture (unset=disabled)
        --linear=         ALSA device identifier for linear (unset=default)
        --add-date        Add current date and time to the filenames
        --convert-linear  Convert linear to flac+u8
        --compress-video  Compress video
        --compress-hifi   Compress hifi
        --resample-hifi   Resample hifi to 10 MSps
        --debug           Show commands executed
        --help            Show usage information
```

#### Example

```text
$ ./local-capture.sh --video=0 --hifi=1 --convert-linear --compress-video --compress-hifi --resample-hifi test
Server started (PID 3854)
server listening on unix:/tmp/tmp.qDMBd0Ynxu/server.sock
PID 3872 is capturing video to test-video.ldf
PID 3874 is capturing hifi to test-hifi.flac
PID 3876 is capturing linear to test-linear.flac, headswitch to test-headswitch.u8
Capture running... Press 'q' to stop the capture.
Capturing for 0m 0s... Buffers:  0%  0%  0%
Capturing for 0m 5s... Buffers:  0%  0%  0%
q
Stopping capture
Encountered 0 overflows during capture
Waiting for writes to finish...
Killing server
Finished!
```
