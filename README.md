## Networking services plugin

This plugin contains code for "stream based" network protocol support on top of the lwIP TCP/IP stack plus some utilities.

"stream based" in this context means that the HAL entry points for streaming are used as the API \(by dynamic pointer swapping on connect\).

#### Server protocols supported:

* Telnet \("raw" mode\).
* Websocket.
* FTP \(requires [SD card plugin](https://github.com/grblHAL/Plugin_SD_card) and card inserted\).
* HTTP \(requires [SD card plugin](https://github.com/grblHAL/Plugin_SD_card) and card inserted\).
* WebDAV - as an extension the HTTP daemon. __Note:__ saving files does not yet work with Windows mounts. Tested ok with WinSCP.
* mDNS - \(multicast DomainName Server\).
* SSDP - \(Simple Service Discovery Protocol\) requires the HTTP daemon running.

The mDNS and SSDP protocols uses UPD multicast/unicast transmission of data and not all drivers are set up to handle that "out-of-the-box".  
Various amount of manual code changes are needed to make them work, see the [RP2040 readme](https://github.com/grblHAL/RP2040/blob/master/README.md)
and [WebUI patch](https://github.com/grblHAL/Plugin_WebUI/tree/main/lwIP%20patch) for further details.

#### Dependencies:

[lwIP library](http://savannah.nongnu.org/projects/lwip/)

Driver capable of networking and having the required "middleware" layer on top of lwIP.

These drivers has this "middleware" layer, further details for how to configure networking can be found on the driver page:

* [iMXRT1062](https://github.com/grblHAL/IMXRT1062) for Teensy 4.1, cabled Ethernet.

* [STM32F7xx](https://github.com/grblHAL/STM32F7xx) for Nucleo-F756ZG, cabled Ethernet.

* [RP2040](https://github.com/grblHAL/RP2040) for Pi Pico W, wireless \(wifi\).

* [MSP432E401Y](https://github.com/grblHAL/MSP432E401Y) for MSP432E401Y LaunchPad, cabled Ethernet.

* [TM4C129](https://github.com/grblHAL/TM4C129) for EK-TM4C1294XL Launchpad, cabled Ethernet.

* [ESP32](https://github.com/grblHAL/ESP32), wireless \(wifi\).

#### Credits:

Parts of _websocket.c_ are pulled from [patch 9525](http://savannah.nongnu.org/patch/?9525) by Sakari Kapanen.

_base64.c_, _sha1.c_ by Brad Conte, pulled from from the same patch as mentioned above.

[multipartparser.c](https://github.com/francoiscolas/multipart-parser) by Fran&ccedil;ois Colas. 

[ftpd code](https://github.com/toelke/lwip-ftpd) by Florian Schulze, Philipp T&ouml;lke and David Olofson \(modified by Terje Io for grblHAL use\).

[httpd code](http://savannah.nongnu.org/projects/lwip/) by Adam Dunkels and Simon Goldschmidt \(modified by Terje Io for grblHAL use\).

[cJSON](https://github.com/DaveGamble/cJSON) Dave Gamble and cJSON contributors.

_wschat.html_ modified from original by [tutorialspoint.com](https://www.tutorialspoint.com/websockets/websockets_javascript_application.htm), for simple websocket testing \(edit line 103 to set address and port before use\).

__NOTE:__ some drivers uses ports of lwIP provided by the MCU supplier.  
__NOTE:__ this plugin is only for the protocol layer. Driver specific code is required for initialising lwIP and start/stop/polling the services.

---
#### Simple websocket test app:
![Test](media/websocket.png)

---
2022-10-05
