# USB-MITM

## What it Is?
`USB-MITM` is a custom sysmodule for the Nintendo Switch that does the following:
- Forcibly patches the `bInterval` of Nintendo GameCube Adapter USB devices to always be `1` - This enables the USB scheduler to poll these devices once ever millisecond leading to improved input consistency
- Sits between the `HID` and `USB` sysmodules to hijack any request to open a GameCube Adapter interface, allowing us to poll it at 1000hz and then proxy the inputs over to HID at the rate which it would normally
read them (125hz)

## What it Isn't?
Literally anything else. I don't quite know how to explain it. This tool was designed around improving input consistency in SSBU.

### Credits

* [__switchbrew__](https://switchbrew.org/wiki/Main_Page) for the extensive documention of the Switch OS.
* [__devkitPro__](https://devkitpro.org/) for the homebrew compiler toolchain.
* __SciresM__ for his dedicated work on the [Atmosphère](https://github.com/Atmosphere-NX) project, libstratosphere and general helpfulness with all things Switch related.
* __Arte__ for this incredible work in creating the Lossless GameCube adapter, which was used significantly in the testing of this application
* [__ndeadly__'s MissionControl](https://github.com/ndeadly/MissionControl), which served as a base for this project's structure, it was difficult to figure out how to set one of these
    up without accessible documentation, which leads me to
* The __ReSwitched__ Discord server which banned me years ago for something I thought wasn't banworthy who kickstarted my journey to becoming someone competent in this modding hellscape.
