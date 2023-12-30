import asyncio
import websockets
import json

data = {
    "read_samples": 4,
    "speed": 10,
    "gain": 1,
    "calibration_factor": -0.015270548,
    "target_dose_single": 9.5,
    "target_dose_double": 18,
}


async def handle_websocket(websocket, path):
    while True:
        try:
            message = await websocket.recv()
            print(message)
            if message == "get":
                await websocket.send(json.dumps(data))
            elif "set" in message:
                _, variable, value = message.split(":")
                data[variable] = value
            print(f"Settings = {json.dumps(data)}")
        except websockets.exceptions.ConnectionClosedOK:
            print("Client disconnected.")
            break


start_server = websockets.serve(handle_websocket, "localhost", 8765)

asyncio.get_event_loop().run_until_complete(start_server)
asyncio.get_event_loop().run_forever()
