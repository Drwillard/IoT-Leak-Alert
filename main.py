import serial
import time

PORT = "COM5"
BAUD = 115200

ser = serial.Serial(PORT, BAUD, timeout=2)

time.sleep(2)
ser.reset_input_buffer()

ser.write(b"c")

size = None

while True:
    line = ser.readline().decode("ascii", errors="ignore").strip()
    print(line)

    if line.startswith("SIZE:"):
        size = int(line.split(":")[1])

    if line == "DATA_START":
        break

if size is None:
    raise RuntimeError("No image size received")

print(f"Expecting {size} bytes")

jpeg = bytearray()

while len(jpeg) < size:
    chunk = ser.read(size - len(jpeg))

    if not chunk:
        raise RuntimeError(
            f"Timeout: only received {len(jpeg)} of {size} bytes"
        )

    jpeg.extend(chunk)

print(f"Received {len(jpeg)} bytes")

print("Start:", jpeg[:2].hex())
print("End:  ", jpeg[-2:].hex())

with open("camera.jpg", "wb") as f:
    f.write(jpeg)

ser.close()