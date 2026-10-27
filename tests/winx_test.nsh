@echo -off
# winx hardware test: run from the SD-card shell. Results go to fs-root\winx_results.txt
# Usage:  winx_test.nsh   (or rename to startup.nsh)
echo ===== winx test ===== > winx_results.txt
ver >> winx_results.txt
echo ----- acpiview -l ----- >> winx_results.txt
acpiview -l >> winx_results.txt
echo ----- date/time (twice, 3 s apart: must advance) ----- >> winx_results.txt
date >> winx_results.txt
time >> winx_results.txt
stall 3000000
time >> winx_results.txt
echo ----- drivers ----- >> winx_results.txt
drivers >> winx_results.txt
echo ----- devices ----- >> winx_results.txt
devices >> winx_results.txt
echo ----- network ----- >> winx_results.txt
ifconfig -l >> winx_results.txt
tftp -? >> winx_results.txt
echo ----- dp ----- >> winx_results.txt
dp -? >> winx_results.txt
dp >> winx_results.txt
echo ----- smbios/memmap ----- >> winx_results.txt
smbiosview -t 1 >> winx_results.txt
memmap >> winx_results.txt
echo ===== done ===== >> winx_results.txt
echo Test finished: see winx_results.txt
