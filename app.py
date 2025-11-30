import streamlit as st
import pandas as pd
import json
import time
import os

st.set_page_config(layout="wide", page_title="Orderbook Engine")

# CONFIG
BOOK_FILE = "book_data.json"
CMD_FILE = "commands.txt"
STATUS_FILE = "status.json"

# --- HELPER FUNCTIONS ---
def load_data():
    if not os.path.exists(BOOK_FILE): return [], [], []
    try:
        with open(BOOK_FILE, "r") as f:
            content = f.read().strip()
            if not content: return [], [], []
            data = json.loads(content)
            return data.get("bids", []), data.get("asks", []), data.get("all_orders", [])
    except:
        return [], [], []

def check_status():
    if not os.path.exists(STATUS_FILE): return None
    try:
        with open(STATUS_FILE, "r") as f:
            content = f.read().strip()
            if not content: return None
            data = json.loads(content)
            return data
    except:
        return None

def send_command(cmd_str):
    try:
        with open(CMD_FILE, "w") as f:
            f.write(cmd_str)
        st.toast(f"📨 Sent: {cmd_str}")
        time.sleep(0.1)
        st.rerun()
    except Exception as e:
        st.error(f"Error: {e}")

# --- SIDEBAR ---
st.sidebar.header("Trading Panel")

action = st.sidebar.selectbox(
    "Select Action",
    ["View Only", "Add Order", "Cancel Order", "Modify Order", "System Check"],
    index=0
)

st.sidebar.markdown("---")

if action == "Add Order":
    st.sidebar.subheader("Add New Order")
    with st.sidebar.form("add_form", clear_on_submit=True):
        type_map = {
            "Limit (GTC)": "GTC", "Fill Or Kill (FOK)": "FOK",
            "Fill And Kill (FAK)": "FAK", "Good For Day (GFD)": "GFD",
            "Market (MKT)": "MKT"
        }
        type_label = st.selectbox("Type", list(type_map.keys()))
        o_type = type_map[type_label]

        side = st.selectbox("Side", ["buy", "sell"])

        if o_type == "MKT":
            st.info("Market Order: Matches immediately.")
            price = 0
        else:
            price = st.number_input("Price", 1, 100000, 100)

        qty = st.number_input("Quantity", 1, 100000, 10)

        if st.form_submit_button("Submit Order"):
            if o_type == "MKT":
                send_command(f"add {o_type} {side} {qty}")
            else:
                send_command(f"add {o_type} {side} {price} {qty}")

elif action == "Cancel Order":
    st.sidebar.subheader("Cancel Order")
    with st.sidebar.form("cancel_form", clear_on_submit=True):
        oid = st.number_input("Order ID", 1, step=1)
        if st.form_submit_button("Cancel"):
            send_command(f"cancel {oid}")

elif action == "Modify Order":
    st.sidebar.subheader("Modify Order")
    with st.sidebar.form("mod_form", clear_on_submit=True):
        oid = st.number_input("Order ID", 1, step=1)
        side = st.selectbox("Side", ["buy", "sell"])
        price = st.number_input("New Price", 1)
        qty = st.number_input("New Qty", 1)
        if st.form_submit_button("Modify"):
            send_command(f"modify {oid} {side} {price} {qty}")

elif action == "System Check":
    st.sidebar.subheader("Diagnostics")
    if st.sidebar.button("Run Integrity Check"):
        if os.path.exists(STATUS_FILE):
            try: os.remove(STATUS_FILE)
            except: pass

        with open(CMD_FILE, "w") as f:
            f.write("check")
        st.toast("Running Check...")

        progress = st.sidebar.progress(0)
        found = False
        for i in range(20):
            time.sleep(0.1)
            progress.progress((i+1)*5)
            status = check_status()
            if status:
                progress.empty()
                if status["status"] == "OK":
                    st.sidebar.success(f"✅ {status['message']}")
                else:
                    st.sidebar.error(f"❌ {status['message']}")
                try: os.remove(STATUS_FILE)
                except: pass
                found = True
                break
        if not found:
            st.sidebar.warning("Timeout: Engine not responding")

# --- MAIN VIEW ---
st.title("⚡ C++ Orderbook Interface")

bids, asks, raw_orders = load_data()
col1, col2 = st.columns(2)

def render_book(data, title, color, is_bid):
    st.subheader(f"{title} ({len(data)})")
    if not data:
        st.caption("Empty")
        return

    df = pd.DataFrame(data)
    df.rename(columns={"quantity": "qty", "quantity_": "qty"}, inplace=True)

    if 'qty' not in df.columns or 'price' not in df.columns:
        st.error("Data Error")
        return

    df = df.sort_values("price", ascending=not is_bid)

    try:
        st.dataframe(
            df.style.bar(subset=['qty'], color=color, vmax=df['qty'].max() if not df.empty else 1),
            use_container_width=True,
            hide_index=True,
            column_config={
                "price": st.column_config.NumberColumn("Price", format="$%d"),
                "qty": st.column_config.NumberColumn("Size"),
            }
        )
    except:
        st.dataframe(df)

with col1:
    render_book(bids, "🟢 Bids (Buy)", "#238636", is_bid=True)

with col2:
    render_book(asks, "🔴 Asks (Sell)", "#da3633", is_bid=False)

st.divider()

# --- DETAILED ORDER VIEW ---
st.subheader("📋 Detailed Order List")
if raw_orders:
    df_raw = pd.DataFrame(raw_orders)
    st.dataframe(
        df_raw,
        use_container_width=True,
        column_config={
            "id": st.column_config.NumberColumn("Order ID", format="%d"),
            "side": "Side",
            "price": st.column_config.NumberColumn("Price", format="$%d"),
            "qty": st.column_config.NumberColumn("Remaining Qty"),
            "type": "Order Type"
        }
    )
else:
    st.caption("No active orders found.")

if action != "System Check":
    time.sleep(1)
    st.rerun()