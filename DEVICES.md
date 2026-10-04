# Airspy

Tested with an Airspy R2 on Linux (ATSC).

```sh
# debian / ubuntu
sudo apt install libairspy0 airspy
sudo usermod -aG plugdev $USER   # log out and back in
# replug the radio, then check it shows up
airspy_info
```

- R2 runs at 10 Msps for TV, Mini not tried yet
- gain is the linearity gain, 0 to 21
- 10 Msps needs a decent CPU, a 2-core laptop couldn't keep up with live ATSC
