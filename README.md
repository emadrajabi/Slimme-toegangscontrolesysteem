# Afstudeerproject: Slim Toegangssysteem

Dit is de centrale repository voor het Slim Toegangssysteem. Het project bevat zowel de firmware voor de ESP32-microcontroller als de backend webapplicatie die op een Raspberry Pi draait.

## Structuur van het Project

De code is opgesplitst in twee hoofdmappen:

### 📁 [/ESP32](./ESP32/)
Bevat de C++ firmware voor de ESP32. Deze code is verantwoordelijk voor de fysieke aansturing van de hardware, zoals de RFID-lezer, de servo en het display.

### 📁 [/RaspberryPi](./RaspberryPi/)
Bevat de Python Flask-webapplicatie. Deze applicatie biedt een beheer-dashboard voor het toevoegen van gebruikers, het beheren van autorisaties en het inzien van de toegangslogboeken.

## Installatie

Volg de instructies in de `README.md`-bestanden binnen elke specifieke map voor de installatie.
