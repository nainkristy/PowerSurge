#!/usr/bin/env python3
"""
Read sensor_data.csv every second and plot BL0937 Power in real time.
Run in a separate terminal while scrape_events.py is running.
"""

import time
import json
import re
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.animation as animation

fig, ax = plt.subplots(figsize=(12, 6))
line, = ax.plot([], [], marker='.', linestyle='-', markersize=2)
ax.set_title('BL0937 Power over time (live)')
ax.set_xlabel('Time')
ax.set_ylabel('Power (W)')
ax.grid(True)


def update_plot(frame):
    """Called every animation frame (every second)."""
    global timestamps, values, line, ax

    try:
        # Read the CSV file (may be open for writing, but that's usually fine)
        df = pd.read_csv('sample_data/sensor_data_mixer_high.csv')
    except (FileNotFoundError, pd.errors.EmptyDataError):
        return line,  # file not ready yet

    if df.empty:
        return line,

    # Convert timestamp column to datetime
    df['timestamp'] = pd.to_datetime(df['timestamp'])


    # Update global lists (in case we want to keep history for later use)
    timestamps = df['timestamp']
    values = df['values']

    # Convert to matplotlib date format
    mpl_ts = [pd.Timestamp(t) for t in timestamps]

    # Update plot data
    line.set_data(mpl_ts, values)

    # Rescale axes
    ax.relim()
    ax.autoscale_view()

    return line,

def main():
    print("Live power graph. Press Ctrl+C in the terminal to stop.")
    ani = animation.FuncAnimation(fig, update_plot, interval=200, blit=False)
    plt.show(block=True)

if __name__ == "__main__":
    main()