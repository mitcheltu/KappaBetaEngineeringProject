# Software

The native offline iPhone/iPad app is in [iOS](iOS/README.md). Open `iOS/KappaBeta.xcodeproj` on a Mac to build it. It shares the Arduino BLE protocol with the [WebApp](WebApp/README.md); disconnect one client before connecting the other. Both apps send explicitly saved profile settings to updated non-demo firmware after the initial history import on each connection, and verify receipt by reading them back. Unsaved defaults/edits are not sent; demo firmware remains compatible without settings transfer. See [the settings protocol](SETTINGS_PROTOCOL.md) for fields, confirmation and device lifetime.

## Original iOS concept

The following is the original planning sketch, not a list of implemented sensor capabilities. The current app matches the WebApp; the wearable supplies PPG rather than ECG and does not implement automatic drowning detection.

```text
WHEN APP OPENS
    Load saved sessions
    Show connection status and session list

WHEN USER CONNECTS
    Find wearable using Bluetooth
    Connect and read device status
    Request list of stored recordings

WHEN USER DOWNLOADS A RECORDING
    Receive and verify data chunks
    Save download progress
    Resume if the connection drops
    Save the completed recording
    Confirm successful receipt to the wearable

WHEN USER OPENS A SESSION
    Show ECG, heart rate, depth, and motion graphs
    Highlight warnings and deployment events

WHEN USER EXPORTS
    Create a shareable file
    Open the iPhone share sheet
```
