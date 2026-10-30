# RCOM Project

Data-Link Protocol - Lab 1, FEUP, 2026/2027.
Class T02, Group G09.

File transfer over a serial connection using SET/UA, Stop & Wait,
RR/REJ, error detection, byte stuffing and limited retransmissions.

## Project Structure

- bin/: Compiled binaries.
- src/application_layer.c: File handling and START, DATA and END packets.
- src/link_layer.c: Data-link protocol, timers and statistics.
- src/main.c: Provided entry point. Must not be changed.
- src/application_layer.h and src/link_layer.h: Provided interfaces.
  Must not be changed.
- src/serial_port.c and src/serial_port.h: Provided serial port library.
  Must not be changed.
- cable/cable.c: Provided virtual cable. Must not be changed.
- Makefile: Provided build and execution targets. Must not be changed.
- penguin.gif: Example file to transfer.

The project stores its headers in src/; there is no include/ directory.

## Requirements

Use Linux, or WSL on Windows. From a Windows PowerShell terminal, enter:
    wsl

If needed, install the build tools and virtual cable dependency on Ubuntu:
    sudo apt update
    sudo apt install build-essential socat

Run all project commands from the repository directory.

## Instructions to Run the Project

### Compile

    make

This builds bin/main and bin/cable.
"Nothing to be done" means the binaries are already up to date.

### Option 1: Two Computers with a Physical Serial Cable

Do not run the virtual cable in this mode.
Confirm which computers and serial ports are physically connected.

Before testing, check communication with GTKTerm on both computers:
    gtkterm

Select each computer's port, 9600 baud, 8 data bits, no parity,
1 stop bit and no flow control. Check characters arrive in both directions.
Close GTKTerm on BOTH computers before running the project.

Compile on both computers:
    make

Start the receiver first, on the computer using /dev/ttyUSB0:
    make run_rx RX_SERIAL_PORT=/dev/ttyUSB0

Then start the transmitter, on the computer using /dev/ttyS0:
    make run_tx TX_SERIAL_PORT=/dev/ttyS0

Alternatively, run the executable directly:
    ./bin/main /dev/ttyUSB0 9600 rx penguin-received.gif
    ./bin/main /dev/ttyS0 9600 tx penguin.gif

Adapt the ports if the computer roles are reversed.
Command-line overrides select physical ports without editing the Makefile.

After both programs finish, compare the hashes.
On the transmitter:
    sha256sum penguin.gif

On the receiver:
    sha256sum penguin-received.gif

The hashes must match. The filenames will be different.

### Option 2: One Computer with Three WSL Terminals

Open three terminals in VS Code. If a terminal starts in PowerShell,
enter WSL in that terminal:
    wsl

In every terminal, enter the project directory. For example:
    cd /mnt/c/Users/melis/RC/RCOM_T02_G09

Adapt this path to the location of your clone. If the terminal is already
inside WSL and the project directory, do not repeat these steps.
On a native Linux computer, use three Linux terminals directly.

Terminal 1: compile, start the virtual cable and leave it running.
    make
    make run_cable

Terminal 2: start the receiver after the cable is ready.
    make run_rx

Terminal 3: start the transmitter after the receiver.
    make run_tx

The receiver uses /tmp/ttyS11 and the transmitter uses /tmp/ttyS10.
These virtual ports are already configured in the original Makefile.

After both programs finish, compare the files in terminal 3:
    make check_files

Or use diff directly:
    diff -s penguin.gif penguin-received.gif

Expected output: "Files ... are identical".
The check_files target masks the diff exit status; read its output.
Use diff directly if an exit status is needed for automated checks.
When finished, stop the virtual cable with Ctrl+C in terminal 1.

## Functional Tests

Run one test at a time and restore the normal channel between tests.
Use the laboratory equipment as instructed by the professor.

1. Normal transfer: start receiver, then transmitter; verify file integrity.
2. Transfer after one timeout: start transmitter first, wait for a timeout,
   then start receiver. Verify recovery and file integrity. Record the actual
   timeout count; repeat with coordinated startup to demonstrate one timeout.
3. No receiver: start only the transmitter. It must stop after exhausting
   its retries and report that the connection was not established.
4. Disconnection: interrupt the serial channel during data transfer for
   about 5 seconds, reconnect, and verify recovery and file integrity.
   Keep the USB adapter connected to its computer.
5. Noise: briefly introduce noise during data transfer, then remove it.
   Check REJ frames, retransmissions, successful completion and file integrity.
6. Duplicates: interrupt only Rx -> Tx during data transfer for about
   5 seconds, then restore it. Check retransmissions, discarded duplicates
   at the receiver and file integrity.

For successful transfers, both programs must finish the connection and
the received file must match the original. Save outputs and hashes.

For virtual cable tests, type these commands in the CABLE terminal,
one command per line followed by Enter:
    off
    on
    ber 0.00005
    ber 0

off disconnects both directions; on reconnects them.
ber introduces random noise; ber 0 disables it.
These are not keyboard shortcuts 0, 1 or 2.
off does not reproduce the one-direction interruption required by test 6.
Virtual tests do not replace the physical laboratory demonstration.

## Parameters and Notes

- Default baud rate: 9600; both endpoints must use the same value.
- Timeout: 4 seconds; up to 3 retransmissions after the initial send.
- The transmitter waits 16 seconds after the final UA to handle repeated DISC.
  This is normal; wait for it to finish.
- A failed transfer can leave a partial output file. Check the current
  attempt's messages and hashes, not just whether the file exists.
- Statistics include frames, timeouts, REJ, duplicates and retransmissions.
  Payload bytes include application headers and START/END packets.
- The general guide also requires an efficiency study and a report.
  Functional tests alone do not replace measurements, analysis and graphs.

## Clean

    make clean

Removes the binaries and penguin-received.gif. Save test evidence first.
Build again with make.

## Members

- Melissa Nizero Cuna - up202209735
- Sofia Miguel Coutinho - up201907520
