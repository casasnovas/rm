#! /bin/sh


echo 1 > /proc/rm/led0
echo 1 > /proc/rm/active
echo "Starting maesures..."
$(sleep 30 & sleep 30) && echo 0 > /proc/rm/led0
echo 0 > /proc/rm/active
echo "Measures finished."