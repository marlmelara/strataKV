# Records docs/media/crash-demo.gif
#
#     vhs scripts/demo.tape
#
# The recording runs the real demo: a real child process, a real SIGKILL, a
# real reopen. --pace only inserts pauses between steps so the result is
# readable at normal speed; it changes nothing about the work being done.

Output docs/media/crash-demo.gif

Set Shell "bash"
Set FontSize 15
Set Width 1060
Set Height 460
Set Padding 22
Set Theme "Catppuccin Mocha"
Set TypingSpeed 45ms

Hide
Type "cd /Users/marlon/Desktop/Projects/strataKV && source .venv/bin/activate && clear"
Enter
Sleep 2s
Show

Type "python scripts/demo_crash_safety.py --pace 0.55"
Sleep 600ms
Enter

Sleep 22s
Sleep 3s
