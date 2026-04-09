#!/usr/bin/env python3
"""
Scrape SSE events from http://10.48.92.42/events and save them to a CSV file.
Runs continuously until stopped (Ctrl+C). Reconnects automatically if the
connection drops.
"""

import csv
import sys
import time
from datetime import datetime
import requests

import json
import re

points = []

DATA_TYPES = {
    "W": 'sensor/Power',
    'A': 'sensor/Current',
    'Wh': 'sensor/Energy',
    'V': 'sensor/Voltage',

}

data_sensor = {
    'W': 0.0,
    'Wh': 0.0,
    'V': 0.0,
    'A': 0.0,
}

def extract_power_from_df(row):
    """Extract power values and timestamps from the DataFrame."""
    if row is None:
        return None
    try:
        data = json.loads(row['data'])
        for key, sensor_name in DATA_TYPES.items():
            if data.get("name_id") == sensor_name:
                data_sensor[key] = float(data.get("value"))
                return True

    except (json.JSONDecodeError, ValueError):
        print(ValueError)
        return False
    return False

def parse_sse_events(response):
    """
    Generator that yields parsed SSE events from a streaming response.
    Each event is a dict with keys: event_type, data, event_id, retry.
    """
    buffer = ""
    for line in response.iter_lines(decode_unicode=True):
        if line is None:
            continue
        buffer += line + "\n"
        if line == "":  # empty line marks the end of an event
            event = {}
            for field_line in buffer.splitlines():
                if field_line == "":
                    continue
                colon_idx = field_line.find(":")
                if colon_idx == -1:
                    continue
                field_name = field_line[:colon_idx]
                field_value = field_line[colon_idx+1:].lstrip()
                if field_name == "event" and field_value != "state":
                    event = None
                    break
                elif field_name == "data":
                    event["data"] = field_value
                elif field_name == "id":
                    event["event_id"] = field_value
                elif field_name == "retry":
                    event["retry"] = field_value


            if event is not None:
                print(event)
                extract_power_from_df(event)
                if response:
                    yield True
            buffer = ""


def write_header_if_needed(csv_file, filename):
    """Write the CSV header only if the file is empty or doesn't exist."""
    try:
        with open(filename, 'r', newline='', encoding='utf-8') as f:
            # Check if file has any content
            if f.read(1):
                return  # file not empty, do not write header
    except FileNotFoundError:
        pass  # file doesn't exist, we'll create it with header

    # File is empty or doesn't exist → write header
    writer = csv.writer(csv_file)
    writer.writerow(['timestamp', 'V', 'A', 'W', 'Wh'])
    csv_file.flush()


def main():
    url = "http://10.83.141.42/events"
    device = input("Device: ")
    csv_filename = "sensor_data_" + device + ".csv"
    reconnect_delay = 5  # seconds to wait before reconnecting

    print(f"Starting continuous capture. Press Ctrl+C to stop.")
    print(f"Writing to: {csv_filename}")

    # Open the CSV file in append mode with buffering disabled (line buffering)
    # so that writes happen immediately.
    with open(csv_filename, 'a', newline='', encoding='utf-8', buffering=1) as csvfile:
        # Write header if the file is empty (or doesn't exist)
        write_header_if_needed(csvfile, csv_filename)
        writer = csv.writer(csvfile)

        # Outer loop to handle reconnections
        while True:
            try:
                print(f"Connecting to {url} ...")
                response = requests.get(url, stream=True, timeout=30)
                response.raise_for_status()

                print("Connected. Reading events...")
                for _ in parse_sse_events(response):
                    timestamp = datetime.now().isoformat()

                    writer.writerow([
                        timestamp,
                        data_sensor['V'],
                        data_sensor['A'],
                        data_sensor['W'],
                        data_sensor['Wh'],
                    ])
                    csvfile.flush()  # ensure data is written to disk

            except requests.exceptions.RequestException as e:
                print(f"\nConnection error: {e}")
                print(f"Reconnecting in {reconnect_delay} seconds...")
                time.sleep(reconnect_delay)
                continue
            except KeyboardInterrupt:
                print("\nInterrupted by user. Exiting...")
                break
            except Exception as e:
                print(f"\nUnexpected error: {e}", file=sys.stderr)
                print(f"Reconnecting in {reconnect_delay} seconds...")
                time.sleep(reconnect_delay)
                continue


if __name__ == "__main__":
    main()