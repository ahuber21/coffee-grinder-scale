import asyncio
import websockets
import json

data = [
    {"target_weight": 9.5},
    {"seconds": 0, "weight": 0},
    {"seconds": 1, "weight": 1},
    {"seconds": 2, "weight": 2},
    {"seconds": 3, "weight": 3},
    {"seconds": 4, "weight": 4},
    {"seconds": 5, "weight": 5},
    {"seconds": 6, "weight": 6},
    {"seconds": 7, "weight": 7},
    {"seconds": 8, "weight": 8},
    {"seconds": 9, "weight": 9},
    {"seconds": 10, "weight": 10},
    {"finalize": True},
]


async def push_data(websocket, path):
    dataIdx = 0
    while True:
        try:
            # Send data to the connected client
            await websocket.send(json.dumps(data[dataIdx]))
            dataIdx = (dataIdx + 1) % len(data)

            # Adjust the interval based on your requirements
            await asyncio.sleep(0.4 if dataIdx > 0 else 1)

        except websockets.exceptions.ConnectionClosedOK:
            print("Client disconnected. Stopping data push.")
            break


start_server = websockets.serve(push_data, "localhost", 8765)

asyncio.get_event_loop().run_until_complete(start_server)
asyncio.get_event_loop().run_forever()
