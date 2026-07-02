import pandas as pd

data = {
    "Referee Name": [
        "AL JASSIM Abdulrahman", "AL TURAIS Khalid", "ARAKI Yusuke", "ARTAN Omar Abdulkadir",
        "ATCHO Pierre", "BARTON Ivan", "BEIDA Dahane", "BENITEZ Juan Gabriel",
        "CALDERON Juan", "CLAUS Raphael", "ELFATH Ismail", "ESKAS Espen",
        "FAGHANI Alireza", "FALCON PEREZ Yael", "FISCHER Drew", "GARAY Cristian",
        "GARCIA Katia", "GHORBAL Mustapha", "HERNANDEZ Alejandro", "HERRERA Dario",
        "JAYED Jalal", "KAWANA-WAUGH Campbell-Kirk", "KOVACS Istvan", "LETEXIER Francois",
        "MA Ning", "MAKHADMEH Adham", "MAKKELIE Danny", "MARCINIAK Szymon",
        "MARIANI Maurizio", "MARTINEZ Hector Said", "MOHAMED Amin", "NATION Oshane",
        "NYBERG Glenn", "OLIVER Michael", "OMAR AL ALI", "ORTEGA Kevin",
        "PENSO Tori", "PINHEIRO Joao", "RAMON ABATTI", "RAMOS Cesar",
        "ROJAS Andres", "SCHAERER Sandro", "TANTASHEV Ilgiz", "TAYLOR Anthony",
        "TEJERA Gustavo", "TELLO Facundo", "TOM Abongile", "TURPIN Clement",
        "VALENZUELA Jesus", "VINCIC Slavko", "WILTON SAMPAIO", "ZWAYER Felix"
    ],
    "Country": [
        "QAT", "KSA", "JPN", "SOM", "GAB", "SLV", "MTN", "PAR", "CRC", "BRA",
        "USA", "NOR", "AUS", "ARG", "CAN", "CHI", "MEX", "ALG", "ESP", "ARG",
        "MAR", "NZL", "ROU", "FRA", "CHN", "JOR", "NED", "POL", "ITA", "HON",
        "EGY", "JAM", "SWE", "ENG", "UAE", "PER", "USA", "POR", "BRA", "MEX",
        "COL", "SUI", "UZB", "ENG", "URU", "ARG", "RSA", "FRA", "VEN", "SVN",
        "BRA", "GER"
    ]
}

df = pd.DataFrame(data)
filename = "referee_names_with_countries.csv"
df.to_csv(filename, index=False)
print(f"Generated {filename}")