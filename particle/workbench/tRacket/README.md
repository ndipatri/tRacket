# tRacket - Tennis Court Occupancy Tracker


<img src="media/tennis_court.png" alt="tRacket Tennis Court" width="400"/>

<img src="media/tRacketDeviceInstalled.jpg" alt="tRacket Device Installed" width="400"/>

<img src="media/tRacketTweetExample.png" alt="tRacket Tweet Example" width="400"/>

This is the Particle Workbench project that contains the code for the courtside tRacket device itself. 

I recommend viewing this README with 'Outline' view turned on: <img src="media/outline.png" alt="Turn on Outline View" width="200"/>



## Architecture

There are three main components to the tRacket system: the tRacket courtside device, an MQTT Broker provided by [Adafruit.io](https://io.adafruit.com/), and an Applet on the [If This Than That (IFTTT)](https://ifttt.com/) Integration Server.

The result is whenever the tRacket device detects the beginning or ending of court occupation, a Tweet is sent.


### tRacket courtside device

The tRacket [Particle](https://www.particle.io/devices/) microcontroller-based courtside device has an LTE wireless radio and can establish an MQTT connection to [Adafruit.io](https://io.adafruit.com/).  Whenever there is a change in occupancy on the court, a message is sent from the tRacket device in this format:

`2023/07/23 05:57:54PM	CoopertownElementary:1`

With a pattern of: |dateTime|tRacketID|:|Occupancy, 1-Occuped, 0-Available|

The tRacket device has a custom 12-volt NiHM battery pack that is charged by a solar panel (as shown in picture above).

### Adafruit.io MQTT Broker

[Adafruit.io](https://io.adafruit.com/) has a $10/month subscription service for devices that need to push data up to the cloud for asynchronous access.  Given the tRacket device is powered by solar and its only network connection is a very cheap LTE radio, we don't want end-consumers to connect directly to the tRacket device.  The Adafruit MQTT Broker is a perfect intermediary.

The tRacket device pushes data up to the 'occupancy' and 'recharge' MQTT topics.  When you push up to Adafruit.io, the topics or 'feeds' are created dynamically. So there is very little you need to configure on Adafruit.io for this to all work.

The 'occupancy' feed contains the occupancy data described previously.  The 'recharge' feed contains a message if the tRacket device gets low on power.  This would be useful for identifying if the solar panels are no longer charging the batteries affectively. 

**My single tRacket device has been running on solar power with no problems for over two years as of July 2023.**


### If This Than That (IFTTT) Integration Server

The If This Than That (IFTTT) Integration Server monitors the Adafruit.io 'occupancy' feed and delivers a message to the [tRacketSensor Twitter Handle](https://twitter.com/tRacketSensor).  Here is an example message:

<img src="media/tRacketTweetExample.png" alt="tRacket Tweet Example" width="400"/>

From the above Tweet history, we can see that the court was occupied but then became available on July 23, 2023 at 06:38PM.  It remained available for 41 minutes until it became occupied again at 07:19PM.

My IFTTT account has been configured with two Applets.  The first detects a change in the Adafruit.io 'occupancy' feed and then passes that data in a webhook back to the second Applet which responds by formulating the above message and posting a Tweet.

Configuring these two Applets it's rather tedious and involves the following details:

#### Two Applets are needed. ####

<img src="media/applet_intro.png" alt="both applets" width="400"/>

#### First Applet ####

'Receives Occupancy Feed update and POSTs to "tRacketTweet" WebHook'

##### First Applet requires three steps: #####

<img src="media/applet1_1.png" alt="both applets" width="400"/>

###### First Applet: If: 'Any new data' #####

<img src="media/applet1_2.png" alt="both applets" width="300"/>

###### First Applet: With: 'History of data' ######

<img src="media/applet1_3.png" alt="both applets" width="300"/>

###### First Applet: When: 'Filter code' ######

<img src="media/applet1_4.png" alt="both applets" width="300"/>

###### First Applet: Then: 'Make a web request' ######

<img src="media/applet1_5.png" alt="both applets" width="300"/>


#### Second Applet ####

'Receives "tRacketTweet" WebHook then tweets to @tRacketSensor'

##### Second Applet requires two steps: #####

<img src="media/applet2_1.png" alt="both applets" width="400"/>

###### Second Applet: If: 'Receive a web request' #####

<img src="media/applet2_2.png" alt="both applets" width="300"/>

###### Second Applet: Then: 'Post a Tweet' ######

<img src="media/applet2_3.png" alt="both applets" width="300"/>



### Parts List ###

#### 12V Solar Panel with Charging Circuit ####
I also used a [Solar Panel kit](https://www.amazon.com/gp/product/B08GX19KT9/ref=ppx_yo_dt_b_search_asin_title?ie=UTF8&psc=1).  The charging terminals from the solar panel attach directly across the 12v NiMH battery pack. (+ to +, - to -)

<img src="media/solar_panel.jpg" alt="solar panel" width="400"/>


#### Rechargeable Battery Pack ####

The battery pack is custom built from [10 1.2V 2800mAH NiHM batteries](https://www.amazon.com/gp/product/B07PJZKB8X/ref=ppx_yo_dt_b_search_asin_title?ie=UTF8&psc=1).  They are soldered in series. 8 are in one pack and 2 are in another.  This arrangement was made to be able to fit the batteries most efficiently in the 3D enclosure. The 2-pack fit under the circuit board. Lots of electrical tape is used to prevent shorts anywhere.

<img src="media/battery_pack.jpg" alt="battery pack" width="400"/>

#### DC-DC Buck Converter ####

The [DC-DC Buck Converter](https://www.amazon.com/Adjustable-Converter-1-5-35v-Efficiency-Regulator/dp/B07C2QF1T1/ref=sr_1_1_sspa?dchild=1&keywords=buck+converter&qid=1609001107&sr=8-1-spons&psc=1&spLa=ZW5jcnlwdGVkUXVhbGlmaWVyPUEzMDVDREpLWDZDM1FUJmVuY3J5cHRlZElkPUEwMzc4NDk1MUpOTlRHQVVaTUNPViZlbmNyeXB0ZWRBZElkPUEwNzUxMTYwMUpFTVFNUDRTQ04wWSZ3aWRnZXROYW1lPXNwX2F0ZiZhY3Rpb249Y2xpY2tSZWRpcmVjdCZkb05vdExvZ0NsaWNrPXRydWU=) is what converts our 12V battery to 3.7V for the micro-controller.  It has a micro-potentiometer that is used to manually adjust down the Vout.

<img src="media/buck_converter.png" alt="buck converter" width="400"/>


#### Particle Boron MicroController ####

The [Boron](https://www.amazon.com/PARTICLE-Development-Endpoints-Circuitry-Bluetooth/dp/B07N96P69L/ref=sr_1_1?dchild=1&keywords=particle+boron&qid=1609000793&sr=8-1) is a Arduiono-based Microcontroller with LTE cellular network connectivity.

<img src="media/boron.png" alt="Boron Microcontroller" width="400"/>

The [tRacket.ino](src/tRacket.ino) file can be used to program Particle Boron.  You will need to create your own [Secrets.h](src/Secrets.h) file which contains the following content:

```
#ifndef SECRETS_H
#define SECRETS_H

// If you check in this code WITH this KEY defined, it will be detected by IO.Adafruit
// and IT WILL BE DISABLED !!!  So please make sure this file is 'ignored' by your
// source code management!
#define AIO_USERNAME "xxxxxx"
#define AIO_KEY "aio_xxxxx"
#define AIO_SERVER      "io.adafruit.com"
#define AIO_SERVERPORT  1883          // use 8883 for SSL

#endif
```

#### Motion Sensor (Brojen BJ-912)  ####

The [Brojen BJ-912](https://www.amazon.com/BROJEN-Brojen-BJ-912-Curtain/dp/B07C53ZS2Y/ref=sr_1_1?dchild=1&keywords=Brojen+BJ-912&qid=1609000375&sr=8-1) motion sensor is placed directly inside the 3D enclosure.  This sensor requires 12V which is why we have a 12V supply.  

Changes need to be made on the inside of the motion sensor. The 'Normally Open (NO)' jumper needs to be removed and the 'LED ON' (i.e. 'Enable LED') jumper wires need to be brought outside of the sensor as shown.  The 'Enable LED' signal is set by the microcontroller as the LED is enabled with tRacket is in 'test' mode.

The motion sensor should be sealed into the main 3D enclosure with exterior silicon-based caulk.  It's important to make this a waterproof seal.

<img src="media/motion_sensor_jumpers.jpg" alt="Motion Sensor Jumpers" width="400"/>

The 'Enable LED' signal is connected to the external bus on the motion sensor as shown:

<img src="media/motion_sensor_bus.jpg" alt="Motion Sensor Bus" width="400"/>


#### Featherwing Doubler ####

<img src="media/featherwing_doubler.png" alt="Featherwing Doubler" width="400"/>

[Featherwing]((https://www.adafruit.com/product/2890)) is a standard for wiring up microcontrollers and peripherals.  Certain connections are common across all three sockets.  The VBAT pin has a max rating of 3.7V.  

#### Current Meter ####

<img src="media/current_meter.png" alt="Current Meter" width="400"/>


We use the [Adafruit INA219 FeatherWing Current Meter](https://learn.adafruit.com/adafruit-ina219-current-sensor-breakout) to warn us when the 12V battery needs to be recharged. 

This comes in the Featherwing form-factor so it fits on the above Featherwing Doubler along with the Particle microcontroller. It also has a small ‘experiment’ area onto which I install transistor for the motion sensor.

We currently report one of two states: ‘RECHARGE’ or ‘GOOD’.  We employ a Schmitt Trigger to determine which state we are in.  This requires two voltage thresholds:  a low (12.2 VDC) and a high (12.6 VDC). This avoids ‘oscillating’ that might happen if the voltage were to hover around a single threshold (e.g. RECHARGE, GOOD, RECHARGE, GOOD, etc.) 

<img src="media/schmitt.png" alt="Schmitt Trigger" width="400"/>


#### Schematic ####

<img src="media/schematic.jpg" alt="both applets" width="400"/>


### 3D STL Files ###

The tRacket device is built using 3D-printed parts. The tRacket device is mounted to a pole with a 3D-printed bracket.  The pole is mounted to a typical tennis court using stainless-steel band clamps.

<img src="media/tracket_device.jpg" alt="tRacket Device" width="400"/>

<img src="media/pole_mount.jpg" alt="pole mount" width="400"/>



[Main Body](3d_prints/tRacket_mainBody.stl)

[Body Mount Left](3d_prints/tRacket_bodyMount1.stl)

[Body Mount Right](3d_prints/tRacket_bodyMount2.stl)

[Bottom Cap](3d_prints/tRacket_bottomCap.stl)

[Top Cap](3d_prints/tRacket_topCap.stl)

[Internal Tray](3d_prints/tRacket_tray.stl)



### Assembly Instructions ###

All seals should be secured with exterior grade silicon-based caulk.  The following pictures are meant to provide guidance on how to assemble tRacket parts.

<img src="media/assembly1.jpg" alt="Assembly 1" width="400"/>

<img src="media/assembly2.jpg" alt="Assembly 2" width="400"/>

<img src="media/assembly3.jpg" alt="Assembly 3" width="400"/>

<img src="media/assembly4.jpg" alt="Assembly 4" width="400"/>

<img src="media/assembly5.jpg" alt="Assembly 5" width="400"/>

<img src="media/assembly6.jpg" alt="Assembly 6" width="400"/>








