# For games/bo2/run_windows.sh: how many screens the PC has, so unattended test runs can open on the second one.
# Prints one line: "screens=<count>".
Add-Type -AssemblyName System.Windows.Forms
"screens=$([System.Windows.Forms.Screen]::AllScreens.Count)"
