import csv

csv_file = None
filename = None

def write_header_if_needed(data):
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
    writer.writerow(data.keys())
    csv_file.flush()

def write_data(data):
    pass

def setup(filename, data):
    global csv_file