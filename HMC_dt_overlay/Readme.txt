To compile device tree for HMC need to enter next command:

dtc -O dtb -o hmc5883-i2c-ovr.dtbo -b 0 -@ hmc5883_ovrl.dts 

Then copy .dtbo file to /boot/overlays folder and add to /boot/config.txt next line:

dtoverlay=hmc5883-i2c-ovr