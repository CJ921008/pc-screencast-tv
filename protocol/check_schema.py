import json
from pathlib import Path
from jsonschema import Draft202012Validator, FormatChecker

schema = json.loads(Path(__file__).with_name("signaling.schema.json").read_text())
Draft202012Validator.check_schema(schema)
validator = Draft202012Validator(schema, format_checker=FormatChecker())
session = "14a92ef7-721c-4670-b65e-cb6791be36dd"
base = dict(protocolVersion=2, requestId=session, timestamp=1)
samples = {
    "hello": dict(computerName="Windows"),
    "capabilities": dict(h264=True, supports1080p30=True),
    "offer": dict(sdp="v=0", video=dict(width=1920, height=1080, fps=30, bitrate=8000000)),
    "answer": dict(sdp="v=0"),
    "ice_candidate": dict(candidate="candidate:1", mid="video", mLineIndex=0),
    "ping": {}, "pong": {}, "disconnect": {},
    "error": dict(code="BUSY", message="busy"),
    "stats": dict(fps=30, bitrate=8000000, rttMs=10, lossPercent=0, decoder="MediaCodec"),
}
for kind, payload in samples.items():
    validator.validate(dict(base, type=kind, payload=dict(sessionId=session, **payload)))
bad = dict(base, type="offer", payload=dict(sessionId=session, **samples["offer"]))
bad["payload"]["video"]["height"] = 720
assert list(validator.iter_errors(bad))
assert list(validator.iter_errors(dict(base, protocolVersion=1, type="ping", payload=dict(sessionId=session))))
assert list(validator.iter_errors(dict(base, type="unknown", payload=dict(sessionId=session))))
print("10 signaling messages and invalid version/resolution/type verified")
