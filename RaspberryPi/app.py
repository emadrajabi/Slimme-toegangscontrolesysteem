# app.py - VOLLEDIG GECORRIGEERDE EN COMPLETE VERSIE
import os
from datetime import datetime
from flask import Flask, render_template, abort, request, redirect, url_for, session
import firebase_admin
from firebase_admin import credentials, firestore
from dotenv import load_dotenv
import requests
from requests.auth import HTTPBasicAuth

# ---- INIT ----
load_dotenv()
app = Flask(__name__)
app.secret_key = os.getenv("FLASK_SECRET_KEY")

# Notion OAuth & Firebase Config
CLIENT_ID = os.getenv("CLIENT_ID")
CLIENT_SECRET = os.getenv("CLIENT_SECRET")
REDIRECT_URI = os.getenv("REDIRECT_URI")

try:
    cred = credentials.Certificate("firebase-credentials.json")
    firebase_admin.initialize_app(cred)
    db = firestore.client()
except Exception as e:
    print(f"FATALE FOUT: Kan Firebase niet initialiseren. Fout: {e}")
    db = None

TABLE_CONFIG = {
    'Personeelsinformatie': ['voornaam', 'achternaam', 'afdeling', 'functie', 'email', 'telefoon', 'adres', 'gekoppelde_uid', 'id'],
    'geautoriseerdPersoneel': ['Naam', 'Afdeling', 'Functie', 'Toegang tot', 'UID'],
    'deurStatus': ['id', 'Status', 'LaatsteUpdate']
}
valid_pages = list(TABLE_CONFIG.keys()) + ["toegangslogboeken"]

# --- AUTHENTICATIE ROUTES ---
@app.route('/login')
def login():
    auth_url = f"https://api.notion.com/v1/oauth/authorize?owner=user&client_id={CLIENT_ID}&redirect_uri={REDIRECT_URI}&response_type=code"
    return render_template('login.html', auth_url=auth_url)

@app.route("/oauth/redirect")
def oauth_redirect():
    code = request.args.get("code")
    if not code: return "Geen authorization code gevonden.", 400

    token_resp = requests.post(
        "https://api.notion.com/v1/oauth/token",
        auth=HTTPBasicAuth(CLIENT_ID, CLIENT_SECRET),
        data={"grant_type": "authorization_code", "code": code, "redirect_uri": REDIRECT_URI}
    )
    if token_resp.status_code != 200: return f"Token-exchange faalde: {token_resp.text}", 400

    session['user_name'] = token_resp.json().get("owner", {}).get("user", {}).get("name", "Gebruiker")
    return redirect(url_for('home'))

@app.route('/logout')
def logout():
    session.clear()
    return redirect(url_for('login'))

# --- HOOFD ROUTES ---
@app.route('/')
def home():
    if 'user_name' not in session: return redirect(url_for('login'))
    return render_template('home.html', user_name=session['user_name'])

@app.route('/data/<page_name>')
def show_data(page_name):
    if 'user_name' not in session: return redirect(url_for('login'))
    if not db: abort(500, description="Kan geen verbinding maken met de Firebase database.")
    if page_name not in valid_pages: abort(404)

    docs_ref = db.collection(page_name).stream()
    raw_data = [doc.to_dict() | {'id': doc.id} for doc in docs_ref]
    headers = TABLE_CONFIG.get(page_name, list(raw_data[0].keys()) if raw_data else [])
    
    if page_name == 'geautoriseerdPersoneel':
        final_data = []
        for item in raw_data:
            final_data.append({
                'id': item.get('id'), 'Naam': f"{item.get('voornaam', '')} {item.get('achternaam', '')}".strip(),
                'Afdeling': item.get('afdeling', ''), 'Functie': item.get('functie', ''),
                'Toegang tot': item.get('toegang_tot', []), 'UID': item.get('id', 'N/A')
            })
    else: final_data = raw_data

    personnel_list, door_list = [], []
    if page_name == 'geautoriseerdPersoneel':
        personnel_docs = db.collection('Personeelsinformatie').stream()
        door_docs = db.collection('deurStatus').stream()
        personnel_list = [doc.to_dict() | {'id': doc.id} for doc in personnel_docs]
        door_list = [doc.to_dict() | {'id': doc.id} for doc in door_docs]
        
    return render_template('data_page.html', page_title=page_name, headers=headers, data=final_data, personnel_list=personnel_list, door_list=door_list)

# --- ADD & DELETE ROUTES (DEZE WAREN VERDWENEN) ---
@app.route('/add/personeel', methods=['POST'])
def add_personeel():
    try:
        data = {'voornaam': request.form.get('voornaam'), 'achternaam': request.form.get('achternaam'),'afdeling': request.form.get('afdeling'), 'functie': request.form.get('functie'),'email': request.form.get('email'), 'telefoon': request.form.get('telefoon'),'adres': request.form.get('adres'), 'gekoppelde_uid': ''}
        db.collection('Personeelsinformatie').add(data)
    except Exception as e: return f"Fout bij opslaan: {e}", 500
    return redirect(url_for('show_data', page_name='Personeelsinformatie'))

@app.route('/add/deur', methods=['POST'])
def add_deur():
    try:
        deur_naam = request.form.get('deur_naam')
        if deur_naam: db.collection('deurStatus').document(deur_naam).set({'Status': 'Gesloten', 'LaatsteUpdate': datetime.now()})
    except Exception as e: return f"Fout: {e}", 500
    return redirect(url_for('show_data', page_name='deurStatus'))

@app.route('/add/autorisatie', methods=['POST'])
def add_autorisatie():
    try:
        personeel_id = request.form.get('personeel_id')
        uid = request.form.get('uid').upper()
        toegangslijst = request.form.getlist('toegang_tot')
        persoon_doc_ref = db.collection('Personeelsinformatie').document(personeel_id)
        persoon_data = persoon_doc_ref.get().to_dict()
        if personeel_id and uid and toegangslijst and persoon_data:
            autorisatie_data = {'voornaam': persoon_data.get('voornaam', ''), 'achternaam': persoon_data.get('achternaam', ''), 'afdeling': persoon_data.get('afdeling', ''), 'functie': persoon_data.get('functie', ''), 'toegang_tot': toegangslijst}
            db.collection('geautoriseerdPersoneel').document(uid).set(autorisatie_data)
            persoon_doc_ref.update({'gekoppelde_uid': uid})
    except Exception as e: return f"Fout bij autoriseren: {e}", 500
    return redirect(url_for('show_data', page_name='geautoriseerdPersoneel'))

@app.route('/delete/<collection_name>/<doc_id>', methods=['POST'])
def delete_document(collection_name, doc_id):
    try:
        if collection_name == 'geautoriseerdPersoneel':
            query = db.collection('Personeelsinformatie').where('gekoppelde_uid', '==', doc_id).limit(1).stream()
            for doc in query: doc.reference.update({'gekoppelde_uid': ''})
        db.collection(collection_name).document(doc_id).delete()
    except Exception as e: return f"Fout bij verwijderen: {e}", 500
    return redirect(url_for('show_data', page_name=collection_name))

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=int(os.getenv("PORT", 5000)), debug=True)
