# PowerSurge
Reduce e-waste and power consumption by detecting issues before they happen.

# Introduction
We are a team of electronics students with an interest in practical applications involving AI and
embedded systems. PowerSurge is designed for a wide customer base, including homeowners
or facilities managers, its main purpose being to safeguard electrical appliances and buyers
wallets.

With our system in place not only do you get the benefits of normal smart plugs but also an
additional safeguard towards power surges and other malfunctions, as well as smart analysis of
the current consumption. Using Edge AI to analyze power consumption enables the device to
deliver better insights, including automatic detection of multiple devices, phantom power,
efficiency degradation, and excessive power consumption, warning the user or performing an
emergency stop before the issue worsens.

We used reverse engineering and consumer-grade smart plugs to create a proof of concept for
real life-saving devices, with the use of ESPHome and custom-made software (Python,
WebApp, C++) and AI, we break limits to implement limits.

# Running the project

## Flashing the outlet

## Building the ESPHome device

## Running the prototype backend

Install the requirements

```py
pip install -r requirements.txt
```

Run the backend

```py
python main.py <api_host> [port]
```
