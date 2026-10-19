#!/usr/bin/env python3
"""Expose pytest JUnit failures as GitHub Actions check annotations."""
from pathlib import Path
import sys
import xml.etree.ElementTree as ET


def escape(value: str) -> str:
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def main() -> int:
    total = 0
    for argument in sys.argv[1:]:
        path = Path(argument)
        if not path.is_file():
            continue
        root = ET.parse(path).getroot()
        failures = []
        for case in root.iter("testcase"):
            problem = case.find("failure")
            if problem is None:
                problem = case.find("error")
            if problem is None:
                continue
            name = "{}.{}".format(case.get("classname", "test"),
                                  case.get("name", "failure"))
            message = problem.text or problem.get("message") or "test failed"
            failures.append((name, message))
            print("::error title={}::{}".format(escape(name), escape(message)))
        if failures:
            print("JUnit failures in {}: {}".format(path, len(failures)))
            for name, message in failures:
                print("--- {} ---".format(name))
                print(message)
            total += len(failures)
    if not total:
        print("JUnit failure summary: no failures found.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
