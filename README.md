# cxadc_vhs_server

A terrible HTTP server made for capturing VHS with two cxadc cards and [cxadc-clockgen-mod](https://github.com/namazso/cxadc-clockgen-mod/).

## Usage

`cxadc_vhs_server version|<port>|unix:<socket>`

> ⚠️ Do not expose the server to the public internet. It is not intended to be secure. 

Endpoints provided:

- GET `/`: Hello world.
- GET `/start`: Start a capture. Returns a JSON with stats. Parameters:
  - `cxadc<number>`: Capture `/dev/cxadc<number>` 
  - `lname=<device name>`: Use `<device name>` ALSA device for capture. Defaults to `hw:CARD=CXADCADCClockGe`
  - `lformat=<format>`: Linear capture format. Defaults to device default.
  - `lrate=<rate>`: Linear capture sample rate. Defaults to device default.
    - `lchannels=<channels>`: Linear capture channels. Defaults to device default.
- GET `/cxadc`: Stream the data being captured from a CX card. Parameters:
  - `<number>`: Access the `<number>`th **captured** card (so if you capture `cxadc1` only, you can access it as 0, **not** 1)
- GET `/linear`: Stream the data being captured from the ALSA device.
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

```text
Usage: local-capture.sh [options] <basepath>
    --video=#         Number of CX card to use for video capture
    --hifi=#          Number of CX card to use for hifi capture
    --linear=<device> ALSA device identifier for linear
    --debug           Show commands executed
    --help            Show usage information
```

Place the script next to the binary.

Example usage:

```text
$ ./local-capture.sh --video=0 --hifi=1 test
Server started (PID 4367)
server listening on port unix:/tmp/tmp.w4oNzQXSGg/server.sock
PID 4381 is capturing video to test-video.u8
PID 4382 is capturing hifi to test-hifi.u8
PID 4383 is capturing linear to test-linear.s24
Capture running...
Press Ctrl+C to stop recording
^C
Stopping capture
Encountered 0 overflows during capture
Waiting for writes to finish...
Killing server
Finished!
```
