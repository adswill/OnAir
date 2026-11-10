# Daily digest (3.9)

Round-4 item 25 (scratchpad r4-per-mode list). Branch feat39-r4.

- Once a day at a set local time (21:00 by default, setting `digestMinute` in minutes after midnight), OnAir sends a **session.digest** event. Its text has one line from every mode that heard something today:
  - ADS-B: aircraft, of them new ever, the farthest
  - AIS: vessels, new ones, the farthest
  - HF digital and CW: new DXCC heard, with the first countries
  - Meteor: passes, lines and lost lines
- Lines appear for the modes that ran in this session. A mode's history store is read when the mode first runs.
- The event is sent only when something wants it. The built-in recipe **Daily digest** (off by default) shows it as a notification. Change its action to ntfy, Telegram or a webhook to get it on the phone. It is not sent on days with nothing heard, and only once per day (also across a restart).

## Tests
`tests/meteor/test_meteor_imagedone.cpp` (digest part): the recipe sends the text once per day and stays quiet on an empty day; the family. The timer and the mode lines were syntax-checked only.
