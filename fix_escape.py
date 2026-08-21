import sys

path = r'F:\Test\ARCs\Project\Functions\CollisionVis.cpp'
with open(path, 'rb') as f:
    data = f.read()

# The bad pattern on line 1345 is: }\"\\;  (bytes: 7d 22 5c 3b)
# It should be: }\n";  (bytes: 7d 5c 6e 22 3b)
old_bytes = b'}\x22\x5c\x3b'   # }"\\;
new_bytes = b'}\x5c\x6e\x22\x3b'  # }\n";

count = data.count(old_bytes)
print(f"Found {count} occurrences of the bad pattern")

# Only fix the one that's inside a string context (near "timestamp")
idx = data.find(b'timestamp')
if idx >= 0:
    # Look for the pattern within 200 bytes after "timestamp"
    region = data[idx:idx+200]
    if old_bytes in region:
        rel_pos = region.find(old_bytes)
        abs_pos = idx + rel_pos
        line_num = data[:abs_pos].count(b'\n') + 1
        print(f"Fixing at line {line_num}, byte {abs_pos}")
        data = data[:abs_pos] + new_bytes + data[abs_pos + len(old_bytes):]
        with open(path, 'wb') as f:
            f.write(data)
        print("Fixed!")
    else:
        print("Pattern not found near timestamp, trying broader search")
        # Fix all occurrences
        data = data.replace(old_bytes, new_bytes)
        with open(path, 'wb') as f:
            f.write(data)
        print("Fixed all occurrences")
else:
    print("timestamp not found")
