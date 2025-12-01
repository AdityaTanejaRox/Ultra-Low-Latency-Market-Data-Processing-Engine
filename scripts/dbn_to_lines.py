import sys
from pathlib import Path

def side_char(val):
    s = str(val)
    if s.upper().startswith("B"): return "B"
    if s.upper().startswith("A"): return "A"
    try:
        v = int(val)
        return "B" if v == 0 else "A"
    except Exception:
        return "B" if s.lower().startswith("bid") else "A"

def main():
    if len(sys.argv) < 3:
        print("usage: python scripts/dbn_to_lines.py <input.dbn> <output.txt>")
        sys.exit(1)

    in_path = Path(sys.argv[1])
    out_path = Path(sys.argv[2])
    if not in_path.exists():
        print(f"ERROR: input not found: {in_path}")
        sys.exit(1)

    try:
        import databento as db
    except Exception as e:
        print("ERROR: Could not import 'databento'. Run: pip install -U databento databento-dbn")
        sys.exit(1)

    count = 0
    with out_path.open("w", encoding="utf-8") as out:
        # Read local DBN file into a DBNStore
        store = db.DBNStore.from_file(str(in_path))  # documented API
        # iterate a DBNStore directly; yields typed records
        # We'll detect MBO-like records by attributes present.
        for rec in store:
            # Skip non-MBO records (we expect fields typical for MBO)
            has_action = hasattr(rec, "action")
            has_order = hasattr(rec, "order_id")
            has_price = hasattr(rec, "price")
            has_size  = hasattr(rec, "size") or hasattr(rec, "qty")
            if not (has_action and has_order and has_price and has_size):
                continue

            # time (ns): prefer ts_event; fallback to ts_recv if needed
            ts_ns = int(getattr(rec, "ts_event", 0) or getattr(rec, "ts_recv", 0) or 0)

            action = str(getattr(rec, "action")).lower()
            order_id = int(getattr(rec, "order_id"))
            price = int(getattr(rec, "price", 0))
            size = int(getattr(rec, "size", 0) or getattr(rec, "qty", 0) or 0)
            side = side_char(getattr(rec, "side", "B"))

            if action.startswith(("add", "a")):
                out.write(f"ADD,{ts_ns},{side},{order_id},{price},{size}\n")
            elif action.startswith(("mod", "r")):
                new_px = int(getattr(rec, "new_price", price))
                new_sz = int(getattr(rec, "new_size", size))
                out.write(f"MOD,{ts_ns},{order_id},{new_px},{new_sz}\n")
            elif action.startswith(("cxl","cancel","d")):
                out.write(f"CXL,{ts_ns},{order_id}\n")
            elif action.startswith(("trd","trade","p")):
                fill_qty = size if size > 0 else int(getattr(rec, "fill_qty", 0))
                out.write(f"TRD,{ts_ns},{order_id},{fill_qty}\n")
            elif action.startswith("clear"):
                out.write(f"CLR,{ts_ns}\n")
            else:
                # ignore others
                continue

            count += 1

    print(f"wrote {count} lines to {out_path}")

if __name__ == "__main__":
    main()
