"""Write pkg/ECI.INI: the original configuration with every engine path pointed at pkg/.

  python tools/make_ini.py <original ECI.INI>
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join(ROOT, "pkg")


def main():
    t = open(sys.argv[1], encoding="latin-1").read().replace("\r\n", "\n")
    t = re.sub(r"^(Path(?:_Rom)?=).*[\\/]([^\\/\n]+)$",
               lambda m: m.group(1) + os.path.join(PKG, m.group(2)).replace("/", "\\"), t, flags=re.M)
    open(os.path.join(PKG, "ECI.INI"), "w", encoding="latin-1", newline="\r\n").write(t)


main()
