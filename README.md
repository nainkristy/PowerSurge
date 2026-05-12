# PowerSurge
Reduce e-waste and power consumption by detecting issues before they happen. This project aims to expand the functionality of standard smart outlets by integrating Edge-AI to learn the normal behaviour of connected devices. The system compares the data collected over time, detecting anomalyes and performing an emegency stop if a malfuction is detected, or just warning users when the efficiency of their electronics starts to decline

Made for the Springineering Challenge at University of Oulu and as a final project for the Hardware Hacking and Reverse Engineering course. The project won best demo award, 3rd overall best project, and the audience vote for sustainability.

# Introduction
We are a team of electronics students with an interest in practical applications involving AI and
embedded systems. PowerSurge is designed for a wide customer base, including homeowners
or facilities managers, its main purpose being to safeguard electrical appliances and buyers
wallets.

With our system in place not only do you get the benefits of normal smart plugs, but also an
additional safeguard towards power surges and other malfunctions, as well as smart analysis of
the current consumption. Using Edge AI to analyze power consumption enables the device to
deliver better insights, including automatic detection of multiple devices, phantom power,
efficiency degradation, and excessive power consumption, warning the user or performing an
emergency stop before the issue worsens.

In practice, the outlet could warn users when devices use more power than expected, like if a fridge didn't close properly or if it needs to be defrosted, or a computer needing to be cleaned. For devices with moving parts, such as a washing machine, it can reduce the damage in case of a mechanical failure by cutting power the moment an anomaly is detected. Furthermore, it can improve old or cheap devices that lack safety features or consume a lot of power when left plugged in.

# Technical Overview

This is a proof of concept made with a [EMAX Smart Outlet](https://www.motonet.fi/tuote/emax-alypistorasia-energiankulutusmittarilla?product=95-02136) flashed with [ESPHome](https://esphome.io/) using [LibreTiny](https://docs.libretiny.eu/). The outlet uses the CB2S low-power Wi-Fi module and HLW8012 power sensor.

The logic for the outlet itself is created using ESPHome's YAML configuration and uses a modified component for the HLW8012 power sensor. This sensor can only measure either voltage or current at a time and needs to keep switching between the 2 modes for a full measurement. Since the voltage from the outlet is somewhat constant, we modified the [hlw8012 library included in ESPHome](github.com/esphome/esphome/blob/dev/esphome/components/hlw8012) to keep current measurement mode active 3 times more than voltage measurement, which reuslts in faster power updates.

The outlet calculates the following data locally: average power, average current, average voltage, phantom power, startup peak current, max_power_variation. All of this can be accessed by connecting directly to the outlet, either via the esphome web-portal or api. The data is used by a backend to detect the device type and power limits. These are then passed back to the outlet which compares them to real-time readings so that it can stop power with as little delay as possible.


# Running the project

## Flashing the outlet

TBD

## Building the ESPHome device

TBD

## Running the prototype backend

Install the requirements:

```py
pip install -r requirements.txt
```

Run the backend:

```py
python main.py <api_host> [port]
```

**! The device running the backend should be on the same network as the outlet.**

# Future plans

