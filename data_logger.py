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

def extract_power_from_df(row):
    """Extract power values and timestamps from the DataFrame."""
    val = None
    if row is None:
        return None
    try:
        data = json.loads(row['data'])
        if data.get('name_id') == 'sensor/BL0937 Power':
            val = data.get('value')
            if val is None:
                state = data.get('state', '')
                m = re.search(r'([\d\.]+)', state)
                if m:
                    val = float(m.group(1))
                else:
                    return None
    except (json.JSONDecodeError, ValueError):
        print(ValueError)
        return None
    return val

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
                response = extract_power_from_df(event)
                if response is not None:
                    yield extract_power_from_df(event)
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
    writer.writerow(['timestamp', 'values'])
    csv_file.flush()


def main():
    url = "http://10.48.92.42/events"
    csv_filename = "sensor_data.csv"
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
                for value in parse_sse_events(response):
                    timestamp = datetime.now().isoformat()

                    writer.writerow([timestamp, value])
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