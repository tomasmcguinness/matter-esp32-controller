# Matter ESP32 Controller

This project sets out to create a Matter Controller which runs on an ESP32. It will give you a web interface for configuring and controlling devices.

## Matter Device support

At present, only On/Off Lights and On/Off Light Switches are supported in the canvas. Any device can be commissioned, but it won't be rendered.

## Flashing

This project is designed to run on the Waveshare ESP32-S3-ETH devkit. I chose this DevKit as it has an integrated Ethernet port, SD Card port and even supports PoE.

To flash, you'll need to both the esp-idf and esp-matter repositories installed.

```
idf.py build flash monitor
```

## Commissioning Devices

At present, only on-network devices can be commissioned. These are devices that have been commissioned using another controller, like iOS Home or Google Home. 

To add a device, use your existing controller to enable Pairing Mode. This will give you a new 12 digit code. Enter that into the Add Device UI.

> [!NOTE]
> I'm trying to improve this flow by building a companion app.




