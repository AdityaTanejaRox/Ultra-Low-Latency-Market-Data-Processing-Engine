#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <map>
#include <deque>
#include <vector>

namespace engine 
{

    // Basic MBO event kinds we’ll support first.
    enum class EventKind : uint8_t
    {
        Add,
        Modify,
        Cancel,
        Trade,
        Clear
    };
    enum class Side : uint8_t
    {
        Bid,
        Ask
    };

    struct MboEvent
    {
        EventKind kind;
        Side side;             // for Add/Modify/Cancel
        uint64_t order_id;     // unique per venue
        int64_t  price;        // price in ticks
        int32_t  qty;          // quantity (lots)
        int64_t  new_price{0}; // for Modify (optional)
        int32_t  new_qty{0};   // for Modify (optional)
        uint64_t match_id{0};  // for Trade (optional)
        uint64_t ts_ns{0};     // event timestamp
    };

    struct LevelView
    {
        int64_t price;
        int64_t total_qty;
        uint32_t orders; // count of orders at this level
    };

    struct BookSnapshot
    {
        std::vector<LevelView> bids; // sorted high -> low
        std::vector<LevelView> asks; // sorted low  -> high
    };

    class OrderBook
    {
    public:
        void on_event(const MboEvent& ev);
        BookSnapshot snapshot_top_n(size_t n) const;

    private:
        struct Order { int64_t price; int32_t qty; Side side; };

        // price -> queue of order_ids (FIFO)
        std::map<int64_t, std::deque<uint64_t>, std::greater<int64_t>> bids_;
        std::map<int64_t, std::deque<uint64_t>, std::less<int64_t>>    asks_;

        // order_id -> Order (for O(1) cancel/modify)
        std::unordered_map<uint64_t, Order> orders_;

        static std::map<int64_t, std::deque<uint64_t>, std::greater<int64_t>>& side_map(Side s, const OrderBook* self);
        static std::map<int64_t, std::deque<uint64_t>, std::greater<int64_t>>& bids_map(const OrderBook* self);
        static std::map<int64_t, std::deque<uint64_t>, std::less<int64_t>>& asks_map(const OrderBook* self);

        void add_order(uint64_t id, Side s, int64_t px, int32_t qty);
        void cancel_order(uint64_t id);
        void modify_order(uint64_t id, int64_t new_px, int32_t new_qty);
        void trade_order(uint64_t id, int32_t fill_qty);
    };

} // namespace engine
