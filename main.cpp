 #include <algorithm>
 #include <atomic>
 #include <cctype>
 #include <chrono>
 #include <condition_variable>
 #include <cstdio>
 #include <ctime>
 #include <fstream>
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

//For Order Type: Market
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

     //Rules for thread safety - disable copy/move
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

     void WriteStatus(bool isOk, const string& message) const;

     void PublishToWeb() const;

     friend void PrintAllOrders(Orderbook &orderBook);
     friend void PrintOrderBook(Orderbook &orderBook);
 };

void Orderbook::PublishToWeb() const
 {
     LevelInfos bidLevels, askLevels;

     struct RawOrderData {
         OrderId id;
         string side;
         Price price;
         Quantity qty;
         string type;
     };
     vector<RawOrderData> allOrders;

     {
         scoped_lock lock(ordersMutex_);

         //aggregated levels
         for (const auto& [price, _] : bids_)
         {
             if (data_.contains(price))
                 bidLevels.push_back({ price, data_.at(price).quantity_ });
         }

         for (const auto& [price, _] : asks_)
         {
             if (data_.contains(price))
                 askLevels.push_back({ price, data_.at(price).quantity_ });
         }

         // individual orders
         for (const auto& [id, entry] : orders_)
         {
             auto order = entry.order_;
             string typeStr;
             switch(order->GetOrderType()) {
                 case OrderType::GoodUntilCancel: typeStr = "GTC"; break;
                 case OrderType::FillAndKill: typeStr = "FAK"; break;
                 case OrderType::FillOrKill: typeStr = "FOK"; break;
                 case OrderType::GoodForDay: typeStr = "GFD"; break;
                 case OrderType::Market: typeStr = "MKT"; break;
             }

             allOrders.push_back({
                 id,
                 (order->GetSide() == Side::Buy ? "Buy" : "Sell"),
                 order->GetPrice(),
                 order->GetRemainingQuantity(),
                 typeStr
             });
         }
     }

     // json
     ofstream outFile("book_data.json");
     if (outFile.is_open())
     {
         outFile << "{ \"bids\": [";
         for (size_t i = 0; i < bidLevels.size(); ++i)
         {
             outFile << "{ \"price\": " << bidLevels[i].price_ << ", \"qty\": " << bidLevels[i].quantity_ << " }";
             if (i < bidLevels.size() - 1) outFile << ",";
         }

         outFile << "], \"asks\": [";
         for (size_t i = 0; i < askLevels.size(); ++i)
         {
             outFile << "{ \"price\": " << askLevels[i].price_ << ", \"qty\": " << askLevels[i].quantity_ << " }";
             if (i < askLevels.size() - 1) outFile << ",";
         }

         outFile << "], \"all_orders\": [";
         for (size_t i = 0; i < allOrders.size(); ++i)
         {
             outFile << "{ \"id\": " << allOrders[i].id
                     << ", \"side\": \"" << allOrders[i].side << "\""
                     << ", \"price\": " << allOrders[i].price
                     << ", \"qty\": " << allOrders[i].qty
                     << ", \"type\": \"" << allOrders[i].type << "\" }";
             if (i < allOrders.size() - 1) outFile << ",";
         }

         outFile << "] }";
         outFile.close();
     }
 }

 void Orderbook::WriteStatus(bool isOk, const string& message) const
 {
     ofstream statusFile("status.json");
     if (statusFile.is_open())
     {
         statusFile << "{ \"status\": \"" << (isOk ? "OK" : "ERROR") << "\", \"message\": \"" << message << "\" }";
         statusFile.close();
     }
 }

 void Orderbook::GoodForDayCycle()
 {
     using namespace chrono;
     const auto end = hours(23);

     while (true)
     {
         const auto now = system_clock::now();
         const auto now_c = system_clock::to_time_t(now);
         tm now_parts; //struct
         localtime_s(&now_parts, &now_c);

         if (now_parts.tm_hour >= end.count())
             now_parts.tm_mday += 1;

         now_parts.tm_hour = (int)end.count();
         now_parts.tm_min = 0;
         now_parts.tm_sec = 0;

         auto next = system_clock::from_time_t(mktime(&now_parts)); // reversed operation
         auto Until = next - now + milliseconds(100);

         // break check
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
     {
         CancelOrderInternal(orderId);
     }
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
         orders.erase(iterator); //key erase moment
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
     auto &data = data_[price]; //direct modification

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
         if (asks_.empty())
             return false;
         const auto [askPrice, _] = *asks_.begin(); // iterator/*pair
         threshold = askPrice; //best ask
     }
     else
     {
         if (bids_.empty())
             return false;
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
             if (askPrice > price)
                 break;

             for (const auto &ask : orders)
             {
                 accumulatedQuantity += ask->GetRemainingQuantity();

                 if (accumulatedQuantity >= quantity)
                     return true;
             }
         }
     }

     else
     {
         Quantity accumulatedQuantity = 0;
         for (const auto &[bidPrice, orders] : bids_)
         {
             if (bidPrice < price)
                 break;

             for (const auto &bid : orders)
             {
                 accumulatedQuantity += bid->GetRemainingQuantity();

                 if (accumulatedQuantity >= quantity)
                     return true;
             }
         }
     }

     return false;
 }

 bool Orderbook::CanMatch(Side side, Price price) const
 {
     if (side == Side::Buy)
     {
         if (asks_.empty())
             return false;

         const auto &[askPrice, _] = *asks_.begin();
         return price >= askPrice; //limit buy
     }

     else
     {
         if (bids_.empty())
             return false;

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

         if (bidPrice < askPrice) //crossing of book
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

         if (bids.empty())
         {
             bids_.erase(bidPrice);
         }

         if (asks.empty())
         {
             asks_.erase(askPrice);
         }
     }

     if (!bids_.empty())
     {
         auto &[_, bids] = *bids_.begin();
         auto &order = bids.front();
         if (order->GetOrderType() == OrderType::FillAndKill)
             CancelOrderInternal(order->GetOrderId());
     }

     if (!asks_.empty())
     {
         auto &[_, asks] = *asks_.begin();
         auto &order = asks.front();
         if (order->GetOrderType() == OrderType::FillAndKill)
             CancelOrderInternal(order->GetOrderId());
     }

     return trades;
 }

 Orderbook::Orderbook() : ordersThread_{ [this]{ GoodForDayCycle(); } }
 { }

 Orderbook::~Orderbook()
 {
     shutdown_.store(true, memory_order_release);
     shutdownCV_.notify_one();
     if (ordersThread_.joinable())
         ordersThread_.join();
 }

 Trades Orderbook::AddOrder(OrderPointer order)
 {
     scoped_lock ordersLock{ ordersMutex_ };

     if (orders_.contains(order->GetOrderId()))
         return { };

     //Based On Types

     //Market Sweeping
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

     //Inserting
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

         if (!orders_.contains(order.GetOrderId()))
             return { };

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

 OrderbookLevelInfos Orderbook::GetOrderInfos() const
 {
     LevelInfos bidInfos, askInfos;

     // Optimized O(L) version
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
                      << " in tree but missing from data_.\\n";
                 isConsistent = false;
                 continue;
             }

             const auto& levelData = data_.at(price);
             if (levelData.count_ != orderList.size())
             {
                 cerr << "Integrity Fail: " << sideName << " price " << price
                      << " count mismatch. Tree: " << orderList.size()
                      << ", Data: " << levelData.count_ << "\\n";
                 isConsistent = false;
             }

             Quantity actualQty = 0;
             for (const auto& order : orderList)
                 actualQty += order->GetRemainingQuantity();

             if (levelData.quantity_ != actualQty)
             {
                 cerr << "Integrity Fail: " << sideName << " price " << price
                      << " quantity mismatch. Tree: " << actualQty
                      << ", Data: " << levelData.quantity_ << "\\n";
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
                  << " in data_ but not in Bids or Asks.\\n";
             isConsistent = false;
         }
     }

     return isConsistent;
 }

 void PrintOrderBook(Orderbook &orderBook)
 {
     // Trigger Web Update
     orderBook.PublishToWeb();

     OrderbookLevelInfos snapshot({}, {});
     {
         scoped_lock lock(orderBook.ordersMutex_);
         snapshot = orderBook.GetOrderInfos();
     }
     const LevelInfos &bidLevels = snapshot.GetBids();
     const LevelInfos &askLevels = snapshot.GetAsks();
     cout << "Bids:";
     if (bidLevels.empty())
         cout << " [None]";
     cout << "\n";
     for (const LevelInfo &level : bidLevels)
     {
         cout << "  Price " << level.price_ << " - Quantity " << level.quantity_ << "\n";
     }
     cout << "Asks:";
     if (askLevels.empty())
         cout << " [None]";
     cout << "\n";
     for (const LevelInfo &level : askLevels)
     {
         cout << "  Price " << level.price_ << " - Quantity " << level.quantity_ << "\n";
     }
 }

 void PrintAllOrders(Orderbook &orderBook)
 {
     vector<tuple<OrderId, Side, Price, Quantity, OrderType>> orders;
     {
         scoped_lock lock(orderBook.ordersMutex_);
         orders.reserve(orderBook.orders_.size());
         for (const auto &[oid, entry] : orderBook.orders_)
         {
             OrderPointer order = entry.order_;
             orders.emplace_back(oid, order->GetSide(), order->GetPrice(), order->GetRemainingQuantity(), order->GetOrderType());
         }
     }
     sort(orders.begin(), orders.end(),
          [](const tuple<OrderId, Side, Price, Quantity, OrderType> &a, const tuple<OrderId, Side, Price, Quantity, OrderType> &b)
          {
              return get<0>(a) < get<0>(b);
          });
     cout << "Active orders:\n";
     if (orders.empty())
     {
         cout << "  [None]\n";
     }
     else
     {
         for (const auto &[orderId, side, price, qty, type] : orders)
         {
             string typeStringing;
             switch (type)
             {
             case OrderType::GoodUntilCancel:
                 typeStringing = "GTC";
                 break;
             case OrderType::GoodForDay:
                 typeStringing = "GFD";
                 break;
             case OrderType::FillAndKill:
                 typeStringing = "FAK";
                 break;
             case OrderType::FillOrKill:
                 typeStringing = "FOK";
                 break;
             case OrderType::Market:
                 typeStringing = "MKT";
                 break;
             }
             cout << "  ID " << orderId << ": " << (side == Side::Buy ? "Buy" : "Sell")
                  << " " << price << " x " << qty << " (" << typeStringing << ")\n";
         }
     }
 }

 enum class Command
{
    Add,
    Cancel,
    Modify,
    Print,
    List,
    Help,
    Exit,
    Check,
    Unknown
};

 Command ParseCommand(const string &cmd)
 {
    string lower = cmd;
    transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "add")    return Command::Add;
    if (lower == "cancel") return Command::Cancel;
    if (lower == "modify") return Command::Modify;
    if (lower == "print" || lower == "book") return Command::Print;
    if (lower == "list")   return Command::List;
    if (lower == "help")   return Command::Help;
    if (lower == "exit" || lower == "quit") return Command::Exit;
    if (lower == "check")  return Command::Check;
    return Command::Unknown;
}

bool ParseOrderTypeToken(const string &token, OrderType &outType)
{
    string typeStr = token;
    transform(typeStr.begin(), typeStr.end(), typeStr.begin(), ::toupper);

    if (typeStr == "GTC")
    {
        outType = OrderType::GoodUntilCancel;
        return true;
    }
    if (typeStr == "GFD")
    {
        outType = OrderType::GoodForDay;
        return true;
    }
    if (typeStr == "FAK")
    {
        outType = OrderType::FillAndKill;
        return true;
    }
    if (typeStr == "FOK")
    {
        outType = OrderType::FillOrKill;
        return true;
    }
    if (typeStr == "MKT" || typeStr == "MARKET")
    {
        outType = OrderType::Market;
        return true;
    }

    return false;
}

bool ParseSide(const string &token, Side &outSide)
{
    string sideStr = token;
    transform(sideStr.begin(), sideStr.end(), sideStr.begin(), ::tolower);

    if (sideStr == "buy")
    {
        outSide = Side::Buy;
        return true;
    }
    if (sideStr == "sell")
    {
        outSide = Side::Sell;
        return true;
    }
    return false;
}

 void PrintTradesForOrder(const Trades &trades, OrderId id, const string &header)
 {
    if (trades.empty())
        return;

    cout << header;
    for (const Trade &tr : trades)
    {
        const TradeInfo &bTrade = tr.GetBidTrade();
        const TradeInfo &at = tr.GetAskTrade();

        Price tradePrice;
        if (bTrade.orderId_ == id)
        {
            tradePrice = at.price_;
        }
        else if (at.orderId_ == id)
        {
            tradePrice = bTrade.price_;
        }
        else
        {
            tradePrice = bTrade.price_;
        }

        cout << "  Buy order " << bTrade.orderId_
             << " and Sell order " << at.orderId_
             << " matched at price " << tradePrice
             << " for quantity " << bTrade.quantity_
             << ".\n";
    }
 }

 int main()
 {
     Orderbook orderbook;

     cout << "======================================================\n";
     cout << "   C++ ORDERBOOK ENGINE RUNNING\n";
     cout << "   Waiting for commands from Streamlit (commands.txt)\n";
     cout << "======================================================\n";

     // Initial publish to ensure file exists
     orderbook.PublishToWeb();

     uint64_t nextOrderId = 1;
     unordered_map<OrderId, Side> idToSide;

     while (true)
     {
         // 1. Poll for Command File
         ifstream cmdFile("commands.txt");
         string input;

         if (cmdFile.is_open())
         {
             stringstream buffer;
             buffer << cmdFile.rdbuf();
             input = buffer.str();
             cmdFile.close();
             // Delete immediately to prevent double-processing
             remove("commands.txt");
         }

         if (input.empty())
         {
             // Sleep to prevent high CPU usage while waiting
             this_thread::sleep_for(chrono::milliseconds(100));
             continue;
         }

         cout << "Received Command: " << input << endl;

         istringstream iss(input);
         vector<string> tokens;
         string token;
         while (iss >> token)
         {
             tokens.push_back(token);
         }

         if (tokens.empty())
             continue;

         Command cmd = ParseCommand(tokens[0]);

         if (cmd == Command::Exit)
         {
             break;
         }

         switch (cmd)
         {
         case Command::Help:
         {
             // Help is less relevant for the web UI but kept for console logs
             cout << "Available commands (via Web UI):\n";
             cout << "  add [type] <buy|sell> <price> <quantity>\n";
             break;
         }
         case Command::Add:
         {
             if (tokens.size() < 3)
             {
                 cout << "Usage: add [type] <buy|sell> <price> <quantity>\n";
                 break;
             }

             OrderType type = OrderType::GoodUntilCancel;
             Side side;
             Price price = 0;
             Quantity quantity = 0;

             bool typeProvided = ParseOrderTypeToken(tokens[1], type);
             size_t idx = typeProvided ? 2 : 1;
             bool isMarket = (type == OrderType::Market);

             size_t neededTokens = idx + (isMarket ? 2 : 3);
             if (tokens.size() < neededTokens)
             {
                 cout << "Usage: add [type] <buy|sell> <price> <quantity>\n";
                 break;
             }

             if (!ParseSide(tokens[idx], side))
             {
                 cout << "Invalid side.\n";
                 break;
             }
             ++idx;

             try
             {
                 if (isMarket)
                 {
                     quantity = static_cast<Quantity>(stoul(tokens[idx]));
                 }
                 else
                 {
                     price = static_cast<Price>(stoi(tokens[idx]));
                     quantity = static_cast<Quantity>(stoul(tokens[idx + 1]));
                 }
             }
             catch (...)
             {
                 cout << "Invalid price or quantity.\n";
                 break;
             }

             OrderId id = nextOrderId++;
             OrderPointer newOrder;

             if (isMarket)
             {
                 newOrder = make_shared<Order>(id, side, quantity);
             }
             else
             {
                 newOrder = make_shared<Order>(type, id, side, price, quantity);
             }

             Trades trades = orderbook.AddOrder(newOrder);

             if (!trades.empty())
             {
                 PrintTradesForOrder(trades, id, "Trade executed:\n");
             }
             else
             {
                 cout << "Order added with no immediate match.\n";
             }

             // Logic to track order side for future Modify commands
             bool fullyFilled = false;

             // Simplified check for fully filled/cancelled status
             if (trades.empty())
             {
                 if (type == OrderType::Market ||
                     type == OrderType::FillOrKill ||
                     type == OrderType::FillAndKill)
                 {
                     fullyFilled = true;
                 }
             }

             // Note: In a real system we'd track filled quantity precisely here.
             // For this UI demo, we assume basic tracking.
             if (!fullyFilled)
             {
                 idToSide[id] = side;
             }

             cout << "Current Order Book:\n";
             PrintOrderBook(orderbook); // Triggers Web Update
             break;
         }
         case Command::Cancel:
         {
             if (tokens.size() < 2)
             {
                 cout << "Usage: cancel <orderId>\n";
                 break;
             }

             OrderId id;
             try
             {
                 id = stoull(tokens[1]);
             }
             catch (...)
             {
                 cout << "Invalid order ID.\n";
                 break;
             }

             orderbook.CancelOrder(id);
             idToSide.erase(id); // Cleanup tracking

             cout << "Order " << id << " cancellation requested.\n";
             PrintOrderBook(orderbook);
             break;
         }
         case Command::Modify:
         {
             if (tokens.size() < 5)
             {
                 cout << "Usage: modify <orderId> [side] <newPrice> <newQuantity>\n";
                 break;
             }

             OrderId id;
             try
             {
                 id = stoull(tokens[1]);
             }
             catch (...)
             {
                 cout << "Invalid order ID.\n";
                 break;
             }

             Side side;
             if (!ParseSide(tokens[2], side))
             {
                 cout << "Invalid Side.\n";
                 break;
             }

             Price newPrice;
             Quantity newQty;
             try
             {
                 newPrice = stoi(tokens[3]);
                 newQty = stoul(tokens[4]);
             }
             catch (...)
             {
                 cout << "Invalid params.\n";
                 break;
             }

             Trades trades = orderbook.ModifyOrder(OrderModify(id, side, newPrice, newQty));

             if (!trades.empty())
             {
                 PrintTradesForOrder(trades, id, "Trade executed due to modification:\n");
             }
             else
             {
                 cout << "Order modified.\n";
             }

             // Update side tracking if needed
             idToSide[id] = side;

             cout << "Current Order Book:\n";
             PrintOrderBook(orderbook);
             break;
         }
         case Command::Print:
         {
             cout << "Current Order Book:\n";
             PrintOrderBook(orderbook);
             break;
         }
         case Command::List:
         {
             PrintAllOrders(orderbook);
             break;
         }
         case Command::Check:
         {
             // === FEEDBACK LOGIC HERE ===
             bool isOk = orderbook.CheckIntegrity();
             if (isOk)
             {
                 cout << "System Integrity Check: PASSED\n";
                 orderbook.WriteStatus(true, "Integrity Check Passed: Data trees match cache.");
             }
             else
             {
                 cout << "System Integrity Check: FAILED\n";
                 orderbook.WriteStatus(false, "Integrity Check Failed: Data mismatch detected!");
             }
             break;
         }
         case Command::Unknown:
         default:
         {
             cout << "Unknown command.\n";
             break;
         }
         }
     }

     return 0;
 }