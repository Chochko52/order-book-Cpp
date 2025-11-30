#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <random>
#include <cassert>

using namespace std;

using Price = int32_t;
using Quantity = uint32_t;
using OrderId = uint64_t;
using OrderIds = vector<OrderId>;

enum class OrderType
{
    GoodUntilCancel,
    FillAndKill,
    FillOrKill,
    GoodForDay,
    Market,
};

enum class Side
{
    Buy,
    Sell
};

struct TradeInfo
{
    OrderId orderId_;
    Price price_;
    Quantity quantity_;
};

class Trade
{
private:
    TradeInfo bidTrade_;
    TradeInfo askTrade_;

public:
    Trade(const TradeInfo &bidTrade, const TradeInfo &askTrade):
       bidTrade_{ bidTrade },
       askTrade_{ askTrade }
    { }

    const TradeInfo &GetBidTrade() const { return bidTrade_; }
    const TradeInfo &GetAskTrade() const { return askTrade_; }
};

using Trades = vector<Trade>;

struct Constants
{
    static const Price InvalidPrice;
};

const Price Constants::InvalidPrice = numeric_limits<Price>::min();

class Order
{
private:
    OrderType orderType_;
    OrderId orderId_;
    Side side_;
    Price price_;
    Quantity initialQuantity_;
    Quantity remainingQuantity_;

public:
    Order(OrderType orderType, OrderId orderId, Side side, Price price, Quantity quantity):
       orderType_{ orderType },
       orderId_{ orderId },
       side_{ side },
       price_{ price },
       initialQuantity_{ quantity },
       remainingQuantity_{ quantity }
    { }

    Order(OrderId orderId, Side side, Quantity quantity):
       Order(OrderType::Market, orderId, side, Constants::InvalidPrice, quantity)
    { }

    OrderId GetOrderId() const { return orderId_; }
    Side GetSide() const { return side_; }
    Price GetPrice() const { return price_; }
    OrderType GetOrderType() const { return orderType_; }
    Quantity GetInitialQuantity() const { return initialQuantity_; }
    Quantity GetRemainingQuantity() const { return remainingQuantity_; }
    Quantity GetFilledQuantity() const { return GetInitialQuantity() - GetRemainingQuantity(); }
    bool IsFilled() const { return GetRemainingQuantity() == 0; }

    void Fill(Quantity quantity)
    {
        if (quantity > GetRemainingQuantity())
            throw logic_error("Order (" + to_string(GetOrderId()) + ") cannot be filled for more than its remaining quantity.");

        remainingQuantity_ -= quantity;
    }

    void ToGoodUntilCancel(Price price)
    {
        if (GetOrderType() != OrderType::Market)
            throw logic_error("Order (" + to_string(GetOrderId()) + ") cannot have its price adjusted, only market orders can.");

        price_ = price;
        orderType_ = OrderType::GoodUntilCancel;
    }
};

using OrderPointer = shared_ptr<Order>;

class OrderModify
{
private:
    OrderId orderId_;
    Price price_;
    Side side_;
    Quantity quantity_;

public:
    OrderModify(OrderId orderId, Side side, Price price, Quantity quantity):
       orderId_{ orderId },
       price_{ price },
       side_{ side },
       quantity_{ quantity }
    { }

    OrderId GetOrderId() const { return orderId_; }
    Price GetPrice() const { return price_; }
    Side GetSide() const { return side_; }
    Quantity GetQuantity() const { return quantity_; }

    OrderPointer NewOrderP(OrderType type) const
    {
        return make_shared<Order>(type, GetOrderId(), GetSide(), GetPrice(), GetQuantity());
    }
};

struct LevelInfo
{
    Price price_;
    Quantity quantity_;
};

using LevelInfos = vector<LevelInfo>;

class OrderbookLevelInfos
{
private:
    LevelInfos bids_;
    LevelInfos asks_;

public:
    OrderbookLevelInfos(const LevelInfos &bids, const LevelInfos &asks):
       bids_{ bids },
       asks_{ asks }
    { }

    const LevelInfos &GetBids() const { return bids_; }
    const LevelInfos &GetAsks() const { return asks_; }
};

using OrderList = list<OrderPointer>;

class Orderbook
{
private:
    struct OrderIterator
    {
        OrderPointer order_{ nullptr };
        OrderList::iterator location_;
    };

    struct LevelData
    {
        Quantity quantity_{};
        Quantity count_{};

        enum class Action
        {
            Add,
            Remove,
            Match,
        };
    };

    unordered_map<Price, LevelData> data_;

    map<Price, OrderList, greater<>> bids_; //descending
    map<Price, OrderList, less<>> asks_; //ascending

    unordered_map<OrderId, OrderIterator> orders_;

    thread ordersThread_;
    mutable mutex ordersMutex_;
    condition_variable shutdownCV_;
    atomic<bool> shutdown_{ false };

    void GoodForDayCycle();
    void CancelOrders(OrderIds orderIds);
    void CancelOrderInternal(OrderId orderId);
    void UpdateLevelData(Price price, Quantity quantity, LevelData::Action action);
    void OnOrderCancelled(OrderPointer order);
    void OnOrderAdded(OrderPointer order);
    void OnOrderMatched(Price price, Quantity quantity, bool isFullyFilled);
    bool CanMatch(Side side, Price price) const;
    bool CanFullyFill(Side side, Price price, Quantity quantity) const;
    Trades MatchOrders();

public:
    Orderbook();
    Orderbook(const Orderbook &) = delete;
    void operator=(const Orderbook &) = delete;
    Orderbook(Orderbook &&) = delete;
    void operator=(Orderbook &&) = delete;
    ~Orderbook();

    Trades AddOrder(OrderPointer order);
    void CancelOrder(OrderId orderId);
    Trades ModifyOrder(OrderModify order);
    size_t Size() const;
    OrderbookLevelInfos GetOrderInfos() const;

    bool CheckIntegrity() const;
};


void Orderbook::GoodForDayCycle()
{
    using namespace chrono;
    const auto end = hours(23);

    while (true)
    {
        const auto now = system_clock::now();
        const auto now_c = system_clock::to_time_t(now);
        tm now_parts;
        localtime_s(&now_parts, &now_c);

        if (now_parts.tm_hour >= end.count())
            now_parts.tm_mday += 1;

        now_parts.tm_hour = (int)end.count();
        now_parts.tm_min = 0;
        now_parts.tm_sec = 0;

        auto next = system_clock::from_time_t(mktime(&now_parts));
        auto Until = next - now + milliseconds(100);

        {
            unique_lock ordersLock{ ordersMutex_ };
            if (shutdown_.load(memory_order_acquire) || shutdownCV_.wait_for(ordersLock, Until) == cv_status::no_timeout)
                return;
        }

        OrderIds orderIds;
        {
            scoped_lock ordersLock{ ordersMutex_ };
            for (const auto &[orderId, entry] : orders_)
            {
                auto order = entry.order_;
                if (order->GetOrderType() == OrderType::GoodForDay)
                {
                    orderIds.push_back(orderId);
                }
            }
        }
        CancelOrders(orderIds);
    }
}

void Orderbook::CancelOrders(OrderIds orderIds)
{
    for (auto orderId : orderIds)
        CancelOrderInternal(orderId);
}

void Orderbook::CancelOrderInternal(OrderId orderId)
{
    if (!orders_.contains(orderId))
        return;

    const auto [order, iterator] = orders_.at(orderId);
    orders_.erase(orderId);

    if (order->GetSide() == Side::Sell)
    {
        auto price = order->GetPrice();
        auto &orders = asks_.at(price);
        orders.erase(iterator);
        if (orders.empty())
            asks_.erase(price);
    }
    else
    {
        auto price = order->GetPrice();
        auto &orders = bids_.at(price);
        orders.erase(iterator);
        if (orders.empty())
            bids_.erase(price);
    }

    OnOrderCancelled(order);
}

void Orderbook::UpdateLevelData(Price price, Quantity quantity, LevelData::Action action)
{
    auto &data = data_[price];

    data.count_ += action == LevelData::Action::Remove ? -1 : action == LevelData::Action::Add ? 1 : 0;
    if (action == LevelData::Action::Remove || action == LevelData::Action::Match)
    {
        data.quantity_ -= quantity;
    }
    else
    {
        data.quantity_ += quantity;
    }

    if (data.count_ == 0)
        data_.erase(price);
}

void Orderbook::OnOrderCancelled(OrderPointer order)
{
    UpdateLevelData(order->GetPrice(), order->GetRemainingQuantity(), LevelData::Action::Remove);
}

void Orderbook::OnOrderAdded(OrderPointer order)
{
    UpdateLevelData(order->GetPrice(), order->GetInitialQuantity(), LevelData::Action::Add);
}

void Orderbook::OnOrderMatched(Price price, Quantity quantity, bool isFullyFilled)
{
    UpdateLevelData(price, quantity, isFullyFilled ? LevelData::Action::Remove : LevelData::Action::Match);
}

bool Orderbook::CanFullyFill(Side side, Price price, Quantity quantity) const
{
    optional<Price> threshold;

    if (side == Side::Buy)
    {
        if (asks_.empty()) return false;
        const auto [askPrice, _] = *asks_.begin();
        threshold = askPrice;
    }
    else
    {
        if (bids_.empty()) return false;
        const auto [bidPrice, _] = *bids_.begin();
        threshold = bidPrice;
    }

    if (!threshold.has_value())
        return false;

    if (side == Side::Buy)
    {
        Quantity accumulatedQuantity = 0;
        for (const auto &[askPrice, orders] : asks_)
        {
            if (askPrice > price) break;
            for (const auto &ask : orders)
            {
                accumulatedQuantity += ask->GetRemainingQuantity();
                if (accumulatedQuantity >= quantity) return true;
            }
        }
    }
    else
    {
        Quantity accumulatedQuantity = 0;
        for (const auto &[bidPrice, orders] : bids_)
        {
            if (bidPrice < price) break;
            for (const auto &bid : orders)
            {
                accumulatedQuantity += bid->GetRemainingQuantity();
                if (accumulatedQuantity >= quantity) return true;
            }
        }
    }

    return false;
}

bool Orderbook::CanMatch(Side side, Price price) const
{
    if (side == Side::Buy)
    {
        if (asks_.empty()) return false;
        const auto &[askPrice, _] = *asks_.begin();
        return price >= askPrice;
    }
    else
    {
        if (bids_.empty()) return false;
        const auto &[bidPrice, _] = *bids_.begin();
        return price <= bidPrice;
    }
}

Trades Orderbook::MatchOrders()
{
    Trades trades;
    trades.reserve(orders_.size());

    while (true)
    {
        if (bids_.empty() || asks_.empty())
            break;

        auto &[bidPrice, bids] = *bids_.begin();
        auto &[askPrice, asks] = *asks_.begin();

        if (bidPrice < askPrice)
            break;

        while (!bids.empty() && !asks.empty())
        {
            auto bid = bids.front();
            auto ask = asks.front();

            Quantity quantity = min(bid->GetRemainingQuantity(), ask->GetRemainingQuantity());

            bid->Fill(quantity);
            ask->Fill(quantity);

            if (bid->IsFilled())
            {
                bids.pop_front();
                orders_.erase(bid->GetOrderId());
            }

            if (ask->IsFilled())
            {
                asks.pop_front();
                orders_.erase(ask->GetOrderId());
            }

            trades.push_back(Trade{
                TradeInfo{ bid->GetOrderId(), bid->GetPrice(), quantity },
                TradeInfo{ ask->GetOrderId(), ask->GetPrice(), quantity }
            });

            OnOrderMatched(bid->GetPrice(), quantity, bid->IsFilled());
            OnOrderMatched(ask->GetPrice(), quantity, ask->IsFilled());
        }

        if (bids.empty()) bids_.erase(bidPrice);
        if (asks.empty()) asks_.erase(askPrice);
    }

    // ======================= FIX IS HERE =======================
    // Using CancelOrderInternal prevents acquiring the lock twice.

    if (!bids_.empty())
    {
        auto &[_, bids] = *bids_.begin();
        auto &order = bids.front();
        if (order->GetOrderType() == OrderType::FillAndKill)
            CancelOrderInternal(order->GetOrderId()); // CHANGED
    }

    if (!asks_.empty())
    {
        auto &[_, asks] = *asks_.begin();
        auto &order = asks.front();
        if (order->GetOrderType() == OrderType::FillAndKill)
            CancelOrderInternal(order->GetOrderId()); // CHANGED
    }

    return trades;
}

Orderbook::Orderbook() : ordersThread_{ [this]{ GoodForDayCycle(); } }
{ }

Orderbook::~Orderbook()
{
    shutdown_.store(true, memory_order_release);
    shutdownCV_.notify_one();
    if(ordersThread_.joinable()) ordersThread_.join();
}

Trades Orderbook::AddOrder(OrderPointer order)
{
    scoped_lock ordersLock{ ordersMutex_ };

    if (orders_.contains(order->GetOrderId()))
        return { };

    if (order->GetOrderType() == OrderType::Market)
    {
        if (order->GetSide() == Side::Buy && !asks_.empty())
        {
            const auto &[worstAsk, _] = *asks_.rbegin();
            order->ToGoodUntilCancel(worstAsk);
        }
        else if (order->GetSide() == Side::Sell && !bids_.empty())
        {
            const auto &[worstBid, _] = *bids_.rbegin();
            order->ToGoodUntilCancel(worstBid);
        }
        else
            return { };
    }

    if (order->GetOrderType() == OrderType::FillAndKill && !CanMatch(order->GetSide(), order->GetPrice()))
        return { };

    if (order->GetOrderType() == OrderType::FillOrKill && !CanFullyFill(order->GetSide(), order->GetPrice(), order->GetInitialQuantity()))
        return { };

    OrderList::iterator iterator;

    if (order->GetSide() == Side::Buy)
    {
        auto &orders = bids_[order->GetPrice()];
        orders.push_back(order);
        iterator = prev(orders.end());
    }
    else
    {
        auto &orders = asks_[order->GetPrice()];
        orders.push_back(order);
        iterator = prev(orders.end());
    }

    orders_.insert({ order->GetOrderId(), OrderIterator{ order, iterator } });
    OnOrderAdded(order);

    return MatchOrders();
}

void Orderbook::CancelOrder(OrderId orderId)
{
    scoped_lock ordersLock{ ordersMutex_ };
    CancelOrderInternal(orderId);
}

Trades Orderbook::ModifyOrder(OrderModify order)
{
    OrderType orderType;
    {
        scoped_lock ordersLock{ ordersMutex_ };
        if (!orders_.contains(order.GetOrderId())) return { };
        const auto &[existingOrder, _] = orders_.at(order.GetOrderId());
        orderType = existingOrder->GetOrderType();
    }

    CancelOrder(order.GetOrderId());
    return AddOrder(order.NewOrderP(orderType));
}

size_t Orderbook::Size() const
{
    scoped_lock ordersLock{ ordersMutex_ };
    return orders_.size();
}

// Optimized GetOrderInfos
OrderbookLevelInfos Orderbook::GetOrderInfos() const
{
    LevelInfos bidInfos, askInfos;
    {
        scoped_lock ordersLock{ ordersMutex_ };
        bidInfos.reserve(bids_.size());
        askInfos.reserve(asks_.size());

        for (const auto& [price, _] : bids_)
        {
            if (data_.contains(price))
                bidInfos.push_back({ price, data_.at(price).quantity_ });
        }

        for (const auto& [price, _] : asks_)
        {
            if (data_.contains(price))
                askInfos.push_back({ price, data_.at(price).quantity_ });
        }
    }
    return OrderbookLevelInfos{ bidInfos, askInfos };
}

bool Orderbook::CheckIntegrity() const
{
    scoped_lock ordersLock{ ordersMutex_ };
    bool isConsistent = true;

    auto CheckSide = [&](const auto& tree, const string& sideName)
    {
        for (const auto& [price, orderList] : tree)
        {
            if (!data_.contains(price))
            {
                cerr << "Integrity Fail: " << sideName << " price " << price
                     << " in tree but missing from data_.\n";
                isConsistent = false;
                continue;
            }

            const auto& levelData = data_.at(price);
            if (levelData.count_ != orderList.size())
            {
                cerr << "Integrity Fail: " << sideName << " price " << price
                     << " count mismatch. Tree: " << orderList.size()
                     << ", Data: " << levelData.count_ << "\n";
                isConsistent = false;
            }

            Quantity actualQty = 0;
            for (const auto& order : orderList)
                actualQty += order->GetRemainingQuantity();

            if (levelData.quantity_ != actualQty)
            {
                cerr << "Integrity Fail: " << sideName << " price " << price
                     << " quantity mismatch. Tree: " << actualQty
                     << ", Data: " << levelData.quantity_ << "\n";
                isConsistent = false;
            }
        }
    };

    CheckSide(bids_, "Bids");
    CheckSide(asks_, "Asks");

    for (const auto& [price, levelData] : data_)
    {
        bool existsInBids = bids_.count(price);
        bool existsInAsks = asks_.count(price);

        if (!existsInBids && !existsInAsks)
        {
            cerr << "Integrity Fail: Phantom price " << price
                 << " in data_ but not in Bids or Asks.\n";
            isConsistent = false;
        }
    }

    return isConsistent;
}

// ==========================================
//           STRESS TEST SUITE
// ==========================================

void StressTest()
{
    cout << "====================================\n";
    cout << "STARTING ORDERBOOK INTEGRITY STRESS TEST\n";
    cout << "====================================\n";

    Orderbook ob;
    uint64_t orderId = 1;
    mt19937 rng(12345);

    cout << "1. Seeding book with 1000 resting SELL orders...\n";
    for(int i=0; i<1000; ++i) {
        Price p = 1000 + (i % 100);
        Quantity q = 100;
        ob.AddOrder(make_shared<Order>(OrderType::GoodUntilCancel, orderId++, Side::Sell, p, q));
    }

    if(!ob.CheckIntegrity()) {
        cerr << "FAILED after seeding.\n";
        exit(1);
    }
    cout << "   Seeding complete. Integrity OK.\n";

    cout << "2. Flooding with 5000 FillAndKill BUY orders (Churn Test)...\n";
    int matchedCount = 0;

    for(int i=0; i<5000; ++i) {
        Price p = 950 + (rng() % 150);
        Quantity q = 10 + (rng() % 200);

        auto trades = ob.AddOrder(make_shared<Order>(OrderType::FillAndKill, orderId++, Side::Buy, p, q));
        if(!trades.empty()) matchedCount++;

        if (i % 500 == 0) {
            if(!ob.CheckIntegrity()) {
                cerr << "FAILED during FAK flood at index " << i << "\n";
                exit(1);
            }
        }
    }

    cout << "   Flood complete. " << matchedCount << " orders triggered matches.\n";
    if(ob.CheckIntegrity()) {
        cout << "   Integrity Check PASSED.\n";
    } else {
        cout << "   Integrity Check FAILED.\n";
        exit(1);
    }

    cout << "3. Testing ModifyOrder integrity...\n";
    OrderId modId = orderId++;
    ob.AddOrder(make_shared<Order>(OrderType::GoodUntilCancel, modId, Side::Sell, 2000, 500));

    ob.ModifyOrder(OrderModify(modId, Side::Sell, 2000, 300));
    if(!ob.CheckIntegrity()) { cerr << "FAILED after Modify 1\n"; exit(1); }

    ob.ModifyOrder(OrderModify(modId, Side::Sell, 2005, 300));
    if(!ob.CheckIntegrity()) { cerr << "FAILED after Modify 2\n"; exit(1); }

    cout << "   Modify tests passed.\n";

    cout << "====================================\n";
    cout << "ALL TESTS PASSED - OPTIMIZATION IS SAFE\n";
    cout << "====================================\n";
}

int main()
{
    StressTest();
    return 0;
}