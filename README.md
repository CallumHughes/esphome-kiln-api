# ESPHome Kiln API

ESP based controller to control a Kiln and program firing schedules.
For the UI part see this repository:
[kiln-controller/ui](https://github.com/kiln-controller/ui)

## Quickstart

Start with the `example_kiln_api.yml` and update the D1/D2/D3/D4/D5 pins if needed
on how you connected your heating element and the max6675.

The most important piece is the climate component with the `kiln` id, that is where
this module interacts with. How you then implement the `sensor` and `heat_output`
doesn't really matter.

If there is some interest I might make some prefabbed PCB's, thumbs up this issue
if interested [#1](https://github.com/kiln-controller/esphome-kiln-api/issues/1)

Upload this using [esphome cli](https://esphome.io/guides/getting_started_command_line/):
```
esphome run example_kiln_api.yml
```

When this upload completes you will find a new Wifi access point, connect to this
and follow the steps on how to connect it to your local Wifi.

If that completes (and your router supports mDNS) you will be able to visit
[http://kiln.local](http://kiln.local)

If that doesn't work check the DHCP leases in your router or something like
[Angry IP scanner](https://angryip.org/download/) to find the device on your network.

Of course you could setup the network with Esphome as well [docs](https://esphome.io/components/wifi/)

## Local test

curl -X POST http://kiln.local/kiln/schedule \
    -H "Content-Type: application/json" \
    -d '{"name":"test","schedule":[[999999,100,1],[10,15,0]]}'

curl -X POST http://kiln.local/number/template_temp/set?value=101

## Credits/Inspiration

* [esphome/esphome](https://www.esphome.io/)
* [jbruce12000/kiln-controller](https://github.com/jbruce12000/kiln-controller/tree/master/public)
* [libretiny-eu/esphome-kickstart](https://github.com/libretiny-eu/esphome-kickstart/tree/master/components/hub_api)
* [ssieb/custom_components](https://github.com/ssieb/custom_components/blob/master/components/web_handler/)