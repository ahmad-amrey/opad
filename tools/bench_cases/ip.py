"""gui_benches cases of the IP area (TODO 11 UI-13/14/54); the bench is app/IpBench.cpp."""

CASES = [
    # The view cube's faces-only switch, the ODA File Converter opt-in through its terms box, the navigation presets named
    # "-style", the commands' records, Help > Third-party licences / About Qt / About. <prefix>.notices.png, .oda-terms.png.
    ("ip", "box", {"OPAD_BENCH_IP": "{prefix}"}),
    # The same in Arabic: every string of app/i18n/ar/ip.json comes through tr(), the licence texts stay left to right.
    ("ip-rtl", "box", {"OPAD_BENCH_IP": "{prefix}", "OPAD_LANG": "ar"}),
]
