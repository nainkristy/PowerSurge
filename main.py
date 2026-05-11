#!/usr/bin/env python3
import asyncio
from aioesphomeapi import APIClient, SensorState, BinarySensorState, NumberState

import csv_writer
import math
import device_if_detection

NAME_TO_INTERNAL = {
    # Commands
    'Device Type': 'device_type',
    'Emergency Stop Power': 'stop_limit',
    'Confidence': 'confidence',
    'Device Running': 'is_device_running',
    # Sensors
    'Average Power': 'avg_pow',
    'Average Current': 'avg_cur',
    'Phantom Power': 'phantom_pwr',
    'Startup Peak Current': 'peak_cur',
    'Max Power Variance': 'max_variance',
    'Voltage': 'V',
}

# Shared state storage
state_data = {k: 0.0 for k in NAME_TO_INTERNAL.values()}

command_keys = {}

device = "Unknown"

def get_phase_angle(cur, vol, power):
    app_pow = cur*vol
    if app_pow > 0:
        # Power Factor (PF) = cos(theta)
        # Clip PF between -1 and 1 to prevent math domain errors from float precision
        pf = max(-1.0, min(1.0, power / app_pow))

        # Calculate angle in degrees
        phase_angle = math.degrees(math.acos(pf))
    else:
        phase_angle = 0.0
    return phase_angle


async def main():
    global device
    client = APIClient("10.87.169.42", 6053, "")

    await client.connect(login=True)

    entities, services = await client.list_entities_services()
    key_to_name = {}

    for entity in entities:
        if entity.name in NAME_TO_INTERNAL:
            key_to_name[entity.key] = NAME_TO_INTERNAL[entity.name]
            # Number or Select entity
            if hasattr(entity, 'key'):
                command_keys[NAME_TO_INTERNAL[entity.name]] = entity.key
        #
        # if "device_running" in entity.name.lower() or "Running" in entity.name:
        #     key_to_name[entity.key] = 'device_running'

    def on_state_update(state):
        if state.key in key_to_name:
            internal_key = key_to_name[state.key]
            # Handle both numeric sensors and binary state
            if isinstance(state, (SensorState, NumberState, BinarySensorState)):
                state_data[internal_key] = state.state

    client.subscribe_states(on_state_update)

    try:
        print("Connected and monitoring...")
        while True:
            await asyncio.sleep(2)

            # Use 'device_running' to gate the output
            if state_data.get('is_device_running'):
                print("RUNNING")
                print("\n--- Device Status (Running) ---")
                phase_angle = get_phase_angle(state_data.get("avg_cur"), state_data.get("V"), state_data.get("avg_pow"))
                print("Phase angle: " + str(phase_angle))
                for key, val in state_data.items():
                    if key != 'is_device_running':
                        print(f"{key}: {val:.3f}")

                device_now = device_if_detection.get_device_type(state_data.get("avg_cur"), state_data.get("avg_pow"),
                                         phase_angle, state_data.get("max_variance"), state_data.get("peak_cur"))
                if device == "Unknown":
                    device = device_now
                    print("Device Type: " + str(device_now))
                    if device:
                        client.select_command(command_keys['device_type'], device)
                        client.number_command(command_keys['stop_limit'], device_if_detection.limits[device])
                    # if state_data.get('confidence') == 100:
                    #     csv_

            else:
                print("OFF")
                device = 'Unknown'
                client.number_command(command_keys['stop_limit'], 3000.0)
                client.select_command(command_keys['device_type'], "Unknown")

    except KeyboardInterrupt:
        await client.disconnect()


if __name__ == "__main__":
    asyncio.run(main())