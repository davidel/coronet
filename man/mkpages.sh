#!/bin/sh

groff -t -e -mandoc -Tascii coronet.3 | col -bx > coronet.txt
groff -t -e -mandoc -Tps coronet.3 | ps2pdf - coronet.pdf
man2html < coronet.3 | sed 's/<BODY>/<BODY text="#0000FF" bgcolor="#FFFFFF" style="font-family: monospace;">/i' > coronet.html

