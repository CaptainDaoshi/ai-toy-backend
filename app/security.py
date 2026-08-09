import hashlib
import hmac


def generate_device_token(device_id: str, server_secret: str) -> str:
    """Derive a stable per-device credential without storing device secrets in MySQL."""
    payload = ("ai-toy-device:v1:" + device_id).encode("utf-8")
    return hmac.new(server_secret.encode("utf-8"), payload, hashlib.sha256).hexdigest()


def verify_device_token(device_id: str, supplied_token: str, server_secret: str) -> bool:
    expected_token = generate_device_token(device_id, server_secret)
    return hmac.compare_digest(supplied_token, expected_token)
