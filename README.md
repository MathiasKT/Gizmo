# Gizmo
MJPEG stream and server for displaying web pages on an ESP32-S3 4B

## Project Structure
1) /libraries - holds arduino libraries need to run it
2) /MJPEG_STREAM - holds code for arduino esp32
3) server.mjs - server code to point to webpage and set up /touch ws and /stream http endpoints

## Setup
1) copy mjpeg_steam and libraries to a esp32
2) edit wifi details, and server address to look for
3) run npm install in server dir
4) runs start.sh in server dir

## TODO
- test git clone works
