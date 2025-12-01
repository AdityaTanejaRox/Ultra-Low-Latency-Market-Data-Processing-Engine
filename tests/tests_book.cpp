#include <gtest/gtest.h>
#include "engine/order_book.hpp"

using namespace engine;

// --- Small helpers so tests don't depend on MboEvent's field order ---
static MboEvent mk_add(uint64_t ts, Side side, uint64_t oid, int64_t px, int qty) {
  MboEvent e{};
  e.kind    = EventKind::Add;
  e.ts_ns   = ts;
  e.side    = side;
  e.order_id= oid;
  e.price   = px;
  e.qty     = qty;
  return e;
}
static MboEvent mk_mod(uint64_t ts, uint64_t oid, int64_t new_px, int new_qty) {
  MboEvent e{};
  e.kind     = EventKind::Modify;
  e.ts_ns    = ts;
  e.order_id = oid;
  e.new_price= new_px;
  e.new_qty  = new_qty;
  return e;
}
static MboEvent mk_cxl(uint64_t ts, uint64_t oid) {
  MboEvent e{};
  e.kind     = EventKind::Cancel;
  e.ts_ns    = ts;
  e.order_id = oid;
  return e;
}
static MboEvent mk_trd(uint64_t ts, uint64_t oid, int fill_qty, Side hit_side = Side::Bid) {
  // If your engine ignores side on trade, it's fine; if it needs it, set it.
  MboEvent e{};
  e.kind     = EventKind::Trade;
  e.ts_ns    = ts;
  e.order_id = oid;
  e.qty      = fill_qty;
  e.side     = hit_side;
  return e;
}
static MboEvent mk_clr(uint64_t ts) {
  MboEvent e{};
  e.kind  = EventKind::Clear;
  e.ts_ns = ts;
  return e;
}

TEST(OrderBook, AddBestBidAsk) {
  OrderBook ob;
  ob.on_event(mk_add(/*ts*/1, Side::Bid, /*oid*/1, /*px*/100, /*qty*/10));
  ob.on_event(mk_add(/*ts*/2, Side::Ask, /*oid*/2, /*px*/105, /*qty*/15));
  auto s = ob.snapshot_top_n(1);
  ASSERT_EQ(s.bids.size(), 1u);
  ASSERT_EQ(s.asks.size(), 1u);
  EXPECT_EQ(s.bids[0].price, 100);
  EXPECT_EQ(s.bids[0].total_qty, 10);
  EXPECT_EQ(s.asks[0].price, 105);
  EXPECT_EQ(s.asks[0].total_qty, 15);
}

TEST(OrderBook, AggregateAndTrade) {
  OrderBook ob;
  ob.on_event(mk_add(1, Side::Bid, 1, 100, 10));
  ob.on_event(mk_add(2, Side::Bid, 3, 100, 20)); // same price level
  auto s1 = ob.snapshot_top_n(1);
  EXPECT_EQ(s1.bids[0].total_qty, 30);

  // Trade against order_id 3 for 5
  ob.on_event(mk_trd(3, 3, /*fill*/5, Side::Bid));
  auto s2 = ob.snapshot_top_n(1);
  EXPECT_EQ(s2.bids[0].total_qty, 25);
}

TEST(OrderBook, ModifyMovesLevel) {
  OrderBook ob;
  ob.on_event(mk_add(1, Side::Bid, 1, 100, 10));
  ob.on_event(mk_mod(2, /*oid*/1, /*new_px*/101, /*new_qty*/10));
  auto s = ob.snapshot_top_n(2);
  ASSERT_FALSE(s.bids.empty());
  EXPECT_EQ(s.bids[0].price, 101);
}

TEST(OrderBook, CancelRemoves) {
  OrderBook ob;
  ob.on_event(mk_add(1, Side::Ask, 2, 105, 15));
  ob.on_event(mk_cxl(2, 2));
  auto s = ob.snapshot_top_n(1);
  EXPECT_TRUE(s.asks.empty());
}

TEST(OrderBook, ClearBook) {
  OrderBook ob;
  ob.on_event(mk_add(1, Side::Bid, 1, 100, 10));
  ob.on_event(mk_add(2, Side::Ask, 2, 105, 15));
  ob.on_event(mk_clr(3));
  auto s = ob.snapshot_top_n(1);
  EXPECT_TRUE(s.bids.empty());
  EXPECT_TRUE(s.asks.empty());
}
