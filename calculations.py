import pandas as pd
import numpy as np

def analyze_power_and_noise(csv_file_path):
    # 1. Load the data
    df = pd.read_csv(csv_file_path)
    df['timestamp'] = pd.to_datetime(df['timestamp'])
    
    # 2. Identify Startup Start (The moment current crosses 0.1A)
    first_nonzero = df[df['A'] > 0.1]['timestamp'].min()
    
    if pd.isna(first_nonzero):
        print("Error: No cycle detected. Current never exceeded 0.1A.")
        return

    # 3. Identify Startup Peak
    max_current = df['A'].max()

    first_peak = df[df['A'] == max_current]['timestamp'].min()
    
    # 4. Identify Cycle End
    # Look for the first time current drops below 0.1A AFTER the startup began
    after_start_df = df[(df['timestamp'] > first_nonzero) & (df['A'] < 0.1)]
    
    if not after_start_df.empty:
        cycle_end_time = after_start_df['timestamp'].min()
    else:
        # If it never drops back down, use the end of the file
        cycle_end_time = df['timestamp'].max()

    # --- Calculations ---
    
    
    # Cycle duration: From 0.1A trigger to the 0.1A drop-off
    cycle_duration = (cycle_end_time - first_nonzero).total_seconds()

    # Filter for "Active" data (only while the machine was actually running)
    active_mask = (df['timestamp'] >= first_nonzero) & (df['timestamp'] <= cycle_end_time)
    df_active = df[active_mask].copy()

    # Average Power: Only wattage above 4W (within the active cycle)
    power_above_4 = df_active.loc[df_active['W'] > 4, 'W']
    avg_power = power_above_4.mean() if not power_above_4.empty else 0.0

    # Phase Angle (Degrees): Using arccos(W / (V * A))
    # We use the active cycle and ensure current is high enough for a stable reading
    valid_phase = (df_active['V'] > 0) & (df_active['A'] > 0.1)
    if valid_phase.any():
        apparent_power = df_active.loc[valid_phase, 'V'] * df_active.loc[valid_phase, 'A']
        pf = (df_active.loc[valid_phase, 'W'] / apparent_power).clip(-1, 1)
        avg_phase_angle = np.degrees(np.arccos(pf)).mean()
    else:
        avg_phase_angle = 0.0

    # Noise Metrics (Current Variation)
    # Calculated only during the active running cycle
    df_active['current_variation'] = df_active['A'].diff().abs()
    avg_noise = df_active['current_variation'].mean()
    max_noise = df_active['current_variation'].max()

    # Print Results
    print(f"{'Metric':<28} | {'Value'}")
    print("-" * 45)
    print(f"{'Average Power (>4W)':<28} | {avg_power:.4f} W")
    print(f"{'Max Current':<28} | {max_current:.4f} A")
    print(f"{'Cycle Duration':<28} | {cycle_duration:.4f} sec")
    print(f"{'Average Phase Angle':<28} | {avg_phase_angle:.2f}°")
    print(f"{'Average Noise (Current Var)':<28} | {avg_noise:.6f} A")
    print(f"{'Max Noise (Current Var)':<28} | {max_noise:.6f} A")

# Run the analysis
analyze_power_and_noise('sensor_data_hair.csv')