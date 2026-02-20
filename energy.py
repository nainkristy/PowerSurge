'''
Laite: päätietokone
Tällä koodilla pistorasioiden datat suoraan tietokantaan
Pistorasiat yhdistyvät automaattisesti TS102:ssa olevan reitittimen tarjoamaan verkkoon, päätietokone samassa verkossa.
'''

import tinytuya
import sqlite3
import time
from datetime import datetime

# Tietokanta
conn = sqlite3.connect('sensor_data.db')
c = conn.cursor()

def insert_data(device_id, sensor, value):
    timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    
    create_table_query = f'''
    CREATE TABLE IF NOT EXISTS [{device_id}] (
        sensor TEXT,
        value REAL,
        timestamp TIMESTAMP
    )
    '''
    c.execute(create_table_query)

    insert_data_query = f'''
    INSERT INTO [{device_id}] (sensor, value, timestamp) VALUES (?, ?, ?)
    '''
    c.execute(insert_data_query, (sensor, value, timestamp))
    conn.commit()

# Pistorasioiden tiedot
devices = [
    {"name": "socket10", "device_id": "bf4dbcb62c16211c48odmw", "ip": "Auto", "local_key": "bj}@-rZ_Uif-Q)8O"},
    {"name": "socket9", "device_id": "bfa9deee7f365057fcvkb9", "ip": "Auto", "local_key": "i;*Rbyx]=]x3/.b'"},
    {"name": "socket8", "device_id": "bf1c2abc3692c1e26boovv", "ip": "Auto", "local_key": "&qwJt+rdLi|{}(&7"},
    {"name": "socket7", "device_id": "bfd361aabccf0054f6nw3t", "ip": "Auto", "local_key": "X0.R.u:~_n`0eSyj"},
    {"name": "socket6", "device_id": "bfbd963782a934dfd6amu2", "ip": "Auto", "local_key": "pmBq;1Ws.oIv3!?>"},
    {"name": "socket5", "device_id": "bfc4b7b85c9d59f0f3s3ec", "ip": "Auto", "local_key": "`J39c~`vkO7&>yaR"},
    {"name": "socket4", "device_id": "bf869915bdeec72275ykag", "ip": "Auto", "local_key": "bKxt?:ONXI!76C>z"},
    {"name": "socket3", "device_id": "bf0a5a6ebead8e7134g008", "ip": "Auto", "local_key": "~8<MP>(}=*DI}Sn6"},
    {"name": "socket2", "device_id": "bf876665fd19a09d32xeuy", "ip": "Auto", "local_key": "`)[Xf?xJU.$c(7P+"},
    {"name": "socket1", "device_id": "bff0020d9895d84361agla", "ip": "Auto", "local_key": "rF?>gmm0#0#t]u.e"},
]

# Funktio pistorasian tilan tarkistamiseen ja tietojen tallentamiseen
def handle_device(device_info):
    try:
        device = tinytuya.OutletDevice(device_info["device_id"], device_info["ip"], device_info["local_key"])
        device.set_version(3.3)
    
        data = device.status()
        print(data)
        if 'dps' in data:
            energy = int(data['dps']['19']) / 10
            current = data['dps']['18']
            voltage = int(data['dps']['20']) / 10

            print(f"{device_info['name']}: Energiankulutus: {energy} Wh, Virta: {current} A, Jännite: {voltage} V")

            # Tallennetaan tietokantaan
           # insert_data(device_info["name"], "energy", energy)
            #insert_data(device_info["name"], "current", float(current))
         #   insert_data(device_info["name"], "voltage", voltage)

            # Laitetaan päälle jos pois päältä
            #if data['dps']['1'] == False:
                #device.turn_on()

    except Exception as e:
        print(f"Virhe laitteessa {device_info['name']}: {e}")

# Pääsilmukka
last_execution = {device["name"]: 0 for device in devices}
interval = 60  # Sekuntia

while True:
    current_time = time.time()
    for device_info in devices:
        if current_time - last_execution[device_info["name"]] >= interval:
            handle_device(device_info)
            last_execution[device_info["name"]] = current_time
    time.sleep(0.5)

# Lopuksi suljetaan tietokanta (jos ohjelma joskus lopetetaan)
# conn.close()
