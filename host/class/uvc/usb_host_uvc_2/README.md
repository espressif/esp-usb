# Espressif Video USB Host driver

Tested on ESP32-P4 and customer's dual camera.

## Limitations
- ISOC transfers only
- The USB device must implement IAD descriptor
- USB reconnections not yet 100% working


## FAQ H265 encoding

Q1: I have two input frames inputI.hevc which is full Intra-coded frame. And a second inputP.hevc which is Predicted frame. How do I decode the second frame to png?

---

### Decoding a P-frame Using an I-frame in HEVC

To decode a P-frame using the corresponding I-frame, you need to combine both frames into a single stream since a P-frame requires the reference frame (I-frame) to be properly decoded. Here’s how you can do it using FFmpeg:

#### Step 1: Combine the I-frame and P-frame into a Single Stream

1. **Concatenate the frames**: Create a text file with the paths to the input HEVC files in the order they should be decoded.

   Create a file `inputs.txt` with the following content:
   ```plaintext
   file 'inputI.hevc'
   file 'inputP.hevc'
   ```

2. **Concatenate the HEVC files using FFmpeg**:
   ```sh
   ffmpeg -f concat -safe 0 -i inputs.txt -c copy combined.hevc
   ```

#### Step 2: Extract and Decode the Second Frame

1. **Decode the combined HEVC stream and extract the second frame**:
   ```sh
   ffmpeg -i combined.hevc -vf "select=eq(n\,1)" -vsync vfr second_frame.png
   ```

#### Explanation

- The `-f concat -safe 0 -i inputs.txt -c copy combined.hevc` command concatenates the I-frame and P-frame into a single HEVC file.
- The `-vf "select=eq(n\,1)" -vsync vfr second_frame.png` command extracts and decodes the second frame (P-frame) from the combined stream.

#### Complete Command Sequence

1. **Create `inputs.txt`**:
   ```plaintext
   file 'inputI.hevc'
   file 'inputP.hevc'
   ```

2. **Combine the HEVC files**:
   ```sh
   ffmpeg -f concat -safe 0 -i inputs.txt -c copy combined.hevc
   ```

3. **Extract and decode the second frame**:
   ```sh
   ffmpeg -i combined.hevc -vf "select=eq(n\,1)" -vsync vfr second_frame.png
   ```

By following these steps, you should be able to decode the second frame (P-frame) using the initial I-frame and convert it to PNG.

---
