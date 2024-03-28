# cxadc_vhs_server

A terrible HTTP server made for capturing VHS with two cxadc cards and [cxadc-clockgen-mod](https://github.com/namazso/cxadc-clockgen-mod/).

## Usage

`cxadc_vhs_server <port>`

Using this, you can start the server on the capture machine:

```text
$ cxadc_vhs_server 8080
```

Then you can queue up the download of the streams:

```text
$ aria2c -Z \
    http://192.168.1.1:8080/linear \
    http://192.168.1.1:8080/cxadc?0 \
    http://192.168.1.1:8080/cxadc?1
```

Then start the capture:

```text
$ curl http://192.168.1.1:8080/start?cxadc0&cxadc1
```

Once you're done, you just need to stop it:

```text
$ curl http://192.168.1.1:8080/stop
```
