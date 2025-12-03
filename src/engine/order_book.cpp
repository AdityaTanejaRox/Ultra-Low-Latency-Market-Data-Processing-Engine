#include "engine/order_book.hpp"
#include <algorithm>
#include <limits>

namespace engine
{

    static void erase_from_queue(std::deque<uint64_t>& dq, uint64_t id)
    {
        auto it = std::find(dq.begin(), dq.end(), id);
        if (it != dq.end()) dq.erase(it);
    }

    std::map<int64_t, std::deque<uint64_t>, std::greater<int64_t>>& OrderBook::bids_map(const OrderBook* self)
    {
        return const_cast<std::map<int64_t, std::deque<uint64_t>, std::greater<int64_t>>&>(self->bids_);
    }

    std::map<int64_t, std::deque<uint64_t>, std::less<int64_t>>& OrderBook::asks_map(const OrderBook* self)
    {
        return const_cast<std::map<int64_t, std::deque<uint64_t>, std::less<int64_t>>&>(self->asks_);
    }

    std::map<int64_t, std::deque<uint64_t>, std::greater<int64_t>>& OrderBook::side_map(Side s, const OrderBook* self)
    {
        return (s == Side::Bid)
            ? bids_map(self)
            : reinterpret_cast<std::map<int64_t, std::deque<uint64_t>, std::greater<int64_t>>&>(asks_map(self));
    }

    void OrderBook::add_order(uint64_t id, Side s, int64_t px, int32_t qty)
    {
        orders_[id] = Order{px, qty, s};
        if (s == Side::Bid)
        {
            bids_[px].push_back(id);
        }
        else
        {
            asks_[px].push_back(id);
        }
    }

    void OrderBook::cancel_order(uint64_t id)
    {
        auto it = orders_.find(id);
        if (it == orders_.end()) return;
        auto s = it->second.side;
        auto px = it->second.price;
        if (s == Side::Bid)
        {
            erase_from_queue(bids_[px], id);
            if (bids_[px].empty()) bids_.erase(px);
        }
        else
        {
            erase_from_queue(asks_[px], id);
            if (asks_[px].empty()) asks_.erase(px);
        }
        orders_.erase(it);
    }

    void OrderBook::modify_order(uint64_t id, int64_t new_px, int32_t new_qty)
    {
        auto it = orders_.find(id);
        if (it == orders_.end()) return;
        auto s = it->second.side;
        auto old_px = it->second.price;

        // If price changes -> remove from old queue and append to new queue tail (loses queue priority)
        if (new_px != old_px)
        {
            if (s == Side::Bid)
            {
                erase_from_queue(bids_[old_px], id);
                if (bids_[old_px].empty()) bids_.erase(old_px);
                bids_[new_px].push_back(id);
            }
            else
            {
                erase_from_queue(asks_[old_px], id);
                if (asks_[old_px].empty()) asks_.erase(old_px);
                asks_[new_px].push_back(id);
            }
            it->second.price = new_px;
        }

        // Update size
        if (new_qty >= 0) it->second.qty = new_qty;
    }

    void OrderBook::trade_order(uint64_t id, int32_t fill_qty)
    {
        auto it = orders_.find(id);
        if (it == orders_.end()) return;
        it->second.qty -= fill_qty;
        if (it->second.qty <= 0) 
        {
            cancel_order(id);
        }
    }

    void OrderBook::on_event(const MboEvent& ev)
    {
        switch (ev.kind)
        {
            case EventKind::Add:
                add_order(ev.order_id, ev.side, ev.price, ev.qty);
                break;
            case EventKind::Modify:
                modify_order(ev.order_id, ev.new_price ? ev.new_price : ev.price, ev.new_qty ? ev.new_qty : ev.qty);
                break;
            case EventKind::Cancel:
                cancel_order(ev.order_id);
                break;
            case EventKind::Trade:
                trade_order(ev.order_id, ev.qty);
                break;
            case EventKind::Clear:  // drop entire side or both
            // clear both for now
                bids_.clear(); asks_.clear(); orders_.clear();
                break;
        }
    }

    BookSnapshot OrderBook::snapshot_top_n(size_t n) const
    {
        BookSnapshot snap;
    // Bids: high -> low
        for (auto it = bids_.begin(); it != bids_.end() && snap.bids.size() < n; ++it)
        {
            int64_t sum = 0;
            for (auto id : it->second)
            {
                auto oit = orders_.find(id);
                if (oit != orders_.end())
                {
                sum += oit->second.qty;
                }
            }
            snap.bids.push_back({it->first, sum, (uint32_t)it->second.size()});
        }
    // Asks: low -> high
        for (auto it = asks_.begin(); it != asks_.end() && snap.asks.size() < n; ++it)
        {
            int64_t sum = 0;
            for (auto id : it->second)
            {
                auto oit = orders_.find(id);
                if (oit != orders_.end())
                {
                    sum += oit->second.qty;
                }
            }
            snap.asks.push_back({it->first, sum, (uint32_t)it->second.size()});
        }
        return snap;
    }

    BookSnapshot OrderBook::snapshot_full() const
    {
        return snapshot_top_n(std::numeric_limits<std::size_t>::max());
    }

} // namespace engine
