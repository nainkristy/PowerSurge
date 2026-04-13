import requests
from bs4 import BeautifulSoup
import csv
import os
import logging

# Configure logging
logging.basicConfig(
    filename='../sample_data/spot_price_scraper.log',
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)
def clear_csv(file_path):
    # Open the file in write mode, which truncates (empties) it
    with open(file_path, "w", newline="") as f:
        pass  # nothing is written, so the file becomes empty
def get_prices():
    try:
        logging.info("Starting price scraping process.")

        # Load the HTML page
        url = 'https://oomi.fi/sahko/sahkosopimukset/aktiivinen/sahkon-spot-hinta/'
        response = requests.get(url)

        if response.status_code != 200:
            logging.error(f"Failed to fetch the page. Status code: {response.status_code}")
            return

        # Create BeautifulSoup object
        soup = BeautifulSoup(response.text, 'html.parser')

        # Find the element that contains the prices
        price_table = soup.find('div', id='spot-prices-table')

        if price_table:
            rows = price_table.find_all('tr')
            prices = []

            for row in rows[1:]:  # Skip header row
                date_element = row.find('td', class_='min-width')
                price_element = row.find('span', class_='price-value')

                if date_element and price_element:
                    date = date_element.text.strip()
                    price = price_element.text.strip()
                    prices.append((date, price))

            logging.info(f"Found {len(prices)} price entries.")

            csv_file = '../sample_data/spot_prices.csv'
            clear_csv(csv_file)
            
            # Clear the CSV file and write new data in one operation
            with open(csv_file, mode='w', newline='', encoding='utf-8') as file:
                writer = csv.writer(file, delimiter=';')
                # Write header row
                writer.writerow(['Date', 'Price'])
                
                if prices:
                    writer.writerows(prices)
                    logging.info(f"{len(prices)} records written to CSV.")
                    print(f"{len(prices)} records written to the CSV file.")
                else:
                    logging.info("No data found to save.")
                    print("No data found to save.")
                    
        else:
            logging.warning("Price table not found on the page.")
            print("Price table not found.")
    except Exception as e:
        logging.exception("An error occurred during scraping.")
        print(f"An error occurred: {e}")

if __name__ == "__main__":
    get_prices()