#!/usr/bin/env python3
"""Require the reviewed ESP-IDF backports in the selected application map."""
import argparse
from pathlib import Path
import re


def require_provider(text: str, symbol: str, object_name: str) -> None:
    matches = re.findall(r"\.text\." + re.escape(symbol) +
        r"\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+([^\n]+)", text)
    selected = [(address, size, source) for address, size, source in matches
                if int(address, 16) and int(size, 16)]
    # Arduino may aggregate sketch objects into objs.a for longer link lines.
    # Both that member and PlatformIO's loose object must identify the same
    # reviewed translation unit; discarded sections still cannot qualify.
    provider = r"(?:(?:^|[/\\])" + re.escape(object_name) + r"$|\.a\(" + re.escape(object_name) + r"\)$)"
    if len(selected) != 1 or not re.search(provider, selected[0][2].strip()):
        raise ValueError(f"{symbol} must resolve once to the shared SDK backport")


def verify_dhcp_map(path: Path) -> None:
    text = path.read_text()
    if re.search(r"liblwip\.a\(dhcpserver\.c\.obj\)", text):
        raise ValueError("unpatched SDK DHCP archive member is linked")
    for symbol in ("dhcps_start", "dhcps_stop"):
        require_provider(text, symbol, "DhcpServer.c.o")


def verify_rtc_map(path: Path) -> None:
    text = path.read_text()
    if re.search(r"libesp_hw_support\.a\(rtc_(?:init|sleep)\.c\.obj\)", text):
        raise ValueError("unpatched SDK RTC archive member is linked")
    require_provider(text, "rtc_init", "RtcInit.c.o")
    require_provider(text, "rtc_sleep_pu", "RtcSleep.c.o")


def verify_tls_map(path: Path) -> None:
    text = path.read_text()
    # GNU maps include extracted members and discarded sections even when
    # none of their code survives --gc-sections. Only a live code contribution
    # establishes that the image contains an old TLS implementation.
    code = re.findall(r"(?m)^[ \t]+\.text(?:\.[^\s]+)?\s+"
                      r"(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+([^\n]+)", text)
    if any(int(address, 16) and int(size, 16) and
           re.search(r"libmbedtls(?:_2)?\.a\(ssl_tls\.c\.obj\)", source)
           for address, size, source in code):
        raise ValueError("unpatched SDK TLS archive member contributes live code")
    # Cardputer and the non-application images do not use TLS. Any image which
    # starts a TLS handshake must select the corrected public entry points.
    sections = re.findall(r"\.text\.mbedtls_ssl_(?:handshake|write_finished|parse_finished)"
                          r"\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)", text)
    if any(int(address, 16) and int(size, 16) for address, size in sections):
        for symbol in ("mbedtls_ssl_write_finished", "mbedtls_ssl_parse_finished"):
            require_provider(text, symbol, "Tls.c.o")


def verify_sdk_map(path: Path, mode: str = "standalone") -> None:
    verify_rtc_map(path)
    verify_tls_map(path)
    if mode != "launcher":
        verify_dhcp_map(path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--map", required=True, type=Path, help="linker map to inspect for selected backport objects")
    parser.add_argument("--mode", choices=["standalone", "launcher", "rnode"], default="standalone",
                        help="image role (default: standalone); launcher skips the DHCP provider check")
    args = parser.parse_args()
    verify_sdk_map(args.map, args.mode)
    print("SDK backports: PASS (required definitions and live TLS providers verified)")


if __name__ == "__main__":
    main()
