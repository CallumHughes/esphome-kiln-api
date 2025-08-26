# ESPHome Kiln API

ESP based controller to control a Kiln and program firing schedules.
For the UI part see this repository:
https://github.com/kiln-controller/ui

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