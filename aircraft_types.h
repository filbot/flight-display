// Aircraft type code to friendly name lookup
// Sorted by ICAO code for binary search. Do not reorder manually.

#pragma once

#include <Arduino.h>

// Rich dataset: ICAO/IATA/manufacturer/model/max seats
struct AircraftTypeInfo {
  const char* icao;
  const char* iata;  // informational only — not consulted by lookups (API "t" is ICAO)
  const char* manufacturer;
  const char* model;
  uint16_t maxSeats;  // upper seat count only
};

// Table is sorted by ICAO code (case-insensitive) with unique keys to enable
// binary search. Keys must be <= 7 characters: FlightInfo.typeCode is char[8],
// so the API value is truncated before lookup and a longer key can never match
// (tools/hosttest/test_types.cpp enforces this). Variant sub-types sharing an ICAO code (freighter/winglet)
// are not listed separately — the "t" field cannot distinguish them.
static const AircraftTypeInfo kTypeInfo[] = {
  { "A10", "", "Fairchild", "A-10", 0 },
  { "A124", "", "Antonov", "An-124 Ruslan", 0 },
  { "A139", "", "AgustaWestland", "AW139", 15 },
  { "A169", "", "AgustaWestland", "AW169", 10 },
  { "A189", "", "AgustaWestland", "AW189", 19 },
  { "A19N", "31N", "Airbus", "A319neo", 160 },
  { "A20N", "32N", "Airbus", "A320neo", 194 },
  { "A21N", "32Q", "Airbus", "A321neo", 244 },
  { "A221", "221", "Airbus", "A220-100", 135 },
  { "A223", "223", "Airbus", "A220-300", 160 },
  { "A225", "", "Antonov", "An-225 Mriya", 0 },
  { "A300", "300", "Airbus", "A300", 300 },
  { "A300F", "", "Airbus", "A300 Freighter (pax eq.)", 0 },
  { "A306", "306", "Airbus", "A300-600R", 304 },
  { "A30B", "30B", "Airbus", "A300-600", 300 },
  { "A310", "310", "Airbus", "A310", 220 },
  { "A318", "318", "Airbus", "A318", 107 },
  { "A319", "319", "Airbus", "A319", 156 },
  { "A320", "320", "Airbus", "A320", 186 },
  { "A321", "321", "Airbus", "A321", 236 },
  { "A332", "332", "Airbus", "A330-200", 260 },
  { "A333", "333", "Airbus", "A330-300", 300 },
  { "A337", "", "Airbus", "A330-700 BelugaXL", 0 },
  { "A338", "338", "Airbus", "A330-800neo", 260 },
  { "A339", "339", "Airbus", "A330-900neo", 300 },
  { "A342", "342", "Airbus", "A340-200", 261 },
  { "A343", "343", "Airbus", "A340-300", 295 },
  { "A345", "345", "Airbus", "A340-500", 310 },
  { "A346", "346", "Airbus", "A340-600", 380 },
  { "A359", "359", "Airbus", "A350-900", 350 },
  { "A35F", "", "Airbus", "A350F (pax eq.)", 0 },
  { "A35K", "35K", "Airbus", "A350-1000", 410 },
  { "A388", "388", "Airbus", "A380-800", 615 },
  { "A3ST", "", "Airbus", "BelugaST", 5 },
  { "A400", "", "Airbus", "A400M Atlas", 0 },
  { "A5", "", "ICON", "A-5", 2 },
  { "A748", "HS7", "Hawker Siddeley", "HS 748", 60 },
  { "AA1", "", "Grumman American", "AA-1", 2 },
  { "AA5", "", "Grumman American", "AA-5", 4 },
  { "AC11", "", "Rockwell", "Commander 112", 4 },
  { "AC50", "", "Aero Commander", "500", 6 },
  { "AC56", "", "Aero Commander", "560", 6 },
  { "AC68", "", "Aero Commander", "680FP", 8 },
  { "AC6L", "", "Aero Commander", "680FL", 8 },
  { "AC80", "", "Rockwell", "Turbo 680", 8 },
  { "AC90", "ACT", "Gulfstream/Rockwell", "Turbo Commander 690", 11 },
  { "AC95", "", "Gulfstream/Rockwell", "Jetprop Commander 1000", 11 },
  { "AEST", "", "Piper", "Aerostar", 6 },
  { "AJ27", "C27", "Comac", "ARJ21-700", 90 },
  { "AN12", "ANF", "Antonov", "An-12", 0 },
  { "AN148", "", "Antonov", "An-148", 85 },
  { "AN158", "", "Antonov", "An-158", 99 },
  { "AN2", "", "Antonov", "An-2", 12 },
  { "AN24", "AN4", "Antonov", "An-24", 52 },
  { "AN26", "A26", "Antonov", "An-26", 50 },
  { "AN28", "A28", "Antonov", "An-28", 19 },
  { "AN30", "A30", "Antonov", "An-30", 33 },
  { "AN32", "A32", "Antonov", "An-32", 52 },
  { "AN72", "AN7", "Antonov", "An-72/74", 52 },
  { "AR11", "", "Aeronca", "11 Chief", 2 },
  { "ARJ1", "AR1", "Comac", "ARJ21-700", 90 },
  { "ASTR", "", "IAI", "Astra 1125", 9 },
  { "AT3T", "", "Air Tractor", "AT-402", 1 },
  { "AT43", "AT4", "ATR", "ATR 42-300/320", 50 },
  { "AT44", "", "ATR", "ATR 42-400", 50 },
  { "AT45", "AT5", "ATR", "ATR 42-500", 50 },
  { "AT46", "ATR", "ATR", "ATR 42-600", 50 },
  { "AT5T", "", "Air Tractor", "AT-503", 1 },
  { "AT6T", "", "Air Tractor", "AT-602", 1 },
  { "AT72", "AT7", "ATR", "ATR 72", 78 },
  { "AT73", "ATR", "ATR", "ATR 72-211/212", 74 },
  { "AT75", "ATR", "ATR", "ATR 72-500", 74 },
  { "AT76", "ATR", "ATR", "ATR 72-600", 78 },
  { "AT8T", "", "Air Tractor", "AT-802", 2 },
  { "ATP", "ATP", "BAe", "ATP", 72 },
  { "B06", "", "Bell", "206", 6 },
  { "B18T", "", "Beechcraft", "18 (Turbo)", 8 },
  { "B190", "", "Beechcraft", "1900/1900D", 19 },
  { "B2", "", "Northrop Grumman", "B-2 Spirit", 2 },
  { "B350", "", "Beechcraft", "Super King Air 350", 11 },
  { "B36T", "", "Beechcraft", "Turbine Bonanza 36", 4 },
  { "B37M", "7M7", "Boeing", "737 MAX 7", 172 },
  { "B38M", "7M8", "Boeing", "737 MAX 8", 197 },
  { "B39M", "7M9", "Boeing", "737 MAX 9", 220 },
  { "B3XM", "7MJ", "Boeing", "737 MAX 10", 230 },
  { "B407", "", "Bell", "407", 6 },
  { "B412", "", "Bell", "412", 15 },
  { "B461", "141", "British Aerospace", "BAe 146-100", 82 },
  { "B462", "142", "British Aerospace", "BAe 146-200", 100 },
  { "B463", "143", "British Aerospace", "BAe 146-300", 116 },
  { "B52", "", "Boeing", "B-52 Stratofortress", 8 },
  { "B701", "701", "Boeing", "707-120", 179 },
  { "B703", "703", "Boeing", "707-320", 189 },
  { "B712", "717", "Boeing", "717-200", 134 },
  { "B720", "720", "Boeing", "720", 149 },
  { "B721", "721", "Boeing", "727-100", 131 },
  { "B722", "722", "Boeing", "727-200", 189 },
  { "B731", "731", "Boeing", "737-100", 104 },
  { "B732", "732", "Boeing", "737-200", 130 },
  { "B733", "733", "Boeing", "737-300", 149 },
  { "B734", "734", "Boeing", "737-400", 168 },
  { "B735", "735", "Boeing", "737-500", 132 },
  { "B736", "736", "Boeing", "737-600", 132 },
  { "B737", "737", "Boeing", "737-700", 149 },
  { "B738", "738", "Boeing", "737-800", 189 },
  { "B739", "739", "Boeing", "737-900", 220 },
  { "B741", "741", "Boeing", "747-100", 452 },
  { "B741F", "", "Boeing", "747-100F (pax eq.)", 0 },
  { "B742", "742", "Boeing", "747-200", 452 },
  { "B743", "743", "Boeing", "747-300", 496 },
  { "B744", "744", "Boeing", "747-400", 524 },
  { "B748", "748", "Boeing", "747-8", 467 },
  { "B74R", "74R", "Boeing", "747SR", 550 },
  { "B74S", "74L", "Boeing", "747SP", 313 },
  { "B752", "752", "Boeing", "757-200", 235 },
  { "B753", "753", "Boeing", "757-300", 295 },
  { "B762", "762", "Boeing", "767-200", 255 },
  { "B763", "763", "Boeing", "767-300", 269 },
  { "B764", "764", "Boeing", "767-400ER", 304 },
  { "B772", "772", "Boeing", "777-200", 396 },
  { "B773", "773", "Boeing", "777-300", 451 },
  { "B778", "778", "Boeing", "777-8", 384 },
  { "B779", "779", "Boeing", "777-9", 426 },
  { "B77L", "77L", "Boeing", "777-200LR", 317 },
  { "B77W", "77W", "Boeing", "777-300ER", 451 },
  { "B788", "788", "Boeing", "787-8", 248 },
  { "B789", "789", "Boeing", "787-9", 296 },
  { "B78X", "78X", "Boeing", "787-10", 330 },
  { "BA11", "", "British Aerospace", "BAe 146-100", 82 },
  { "BA12", "", "British Aerospace", "BAe 146-200", 100 },
  { "BA13", "", "British Aerospace", "BAe 146-300", 116 },
  { "BCS1", "221", "Airbus", "A220-100", 135 },
  { "BCS3", "223", "Airbus", "A220-300", 160 },
  { "BE10", "", "Beechcraft", "King Air 100", 9 },
  { "BE18", "", "Beechcraft", "18", 8 },
  { "BE19", "", "Beechcraft", "B19 Musketeer", 4 },
  { "BE20", "", "Beechcraft", "Super King Air 200", 13 },
  { "BE23", "", "Beechcraft", "23 Sundowner", 4 },
  { "BE24", "", "Beechcraft", "24 Sierra", 4 },
  { "BE30", "", "Beechcraft", "Super King Air 300/350", 11 },
  { "BE33", "", "Beechcraft", "Bonanza 33", 4 },
  { "BE35", "", "Beechcraft", "Bonanza 35", 4 },
  { "BE36", "", "Beechcraft", "Bonanza 36", 4 },
  { "BE40", "", "Raytheon/Beech", "Beechjet 400/T-1", 8 },
  { "BE50", "", "Beechcraft", "50 Twin Bonanza", 5 },
  { "BE55", "", "Beechcraft", "Baron 55", 6 },
  { "BE58", "", "Beechcraft", "Baron 58", 6 },
  { "BE60", "", "Beechcraft", "60 Duke", 6 },
  { "BE65", "", "Beechcraft", "65 Queen Air", 9 },
  { "BE70", "", "Beechcraft", "70 Queen Air", 8 },
  { "BE76", "", "Beechcraft", "76 Duchess", 4 },
  { "BE77", "", "Beechcraft", "77 Skipper", 2 },
  { "BE80", "", "Beechcraft", "80 Queen Air", 8 },
  { "BE95", "", "Beechcraft", "95 Travel Air", 4 },
  { "BE99", "", "Beechcraft", "Model 99 Airliner", 17 },
  { "BE9L", "", "Beechcraft", "King Air 90", 9 },
  { "BE9T", "", "Beechcraft", "F90 King Air", 9 },
  { "BELF", "SHB", "Shorts", "SC-5 Belfast", 43 },
  { "BLCF", "", "Boeing", "747 LCF Dreamlifter", 8 },
  { "BT36", "", "Beechcraft", "36 Bonanza", 4 },
  { "C120", "", "Cessna", "120", 2 },
  { "C130", "", "Lockheed Martin", "C-130 Hercules", 92 },
  { "C140", "", "Cessna", "140", 2 },
  { "C150", "", "Cessna", "150", 2 },
  { "C152", "", "Cessna", "152", 2 },
  { "C160", "", "Transall", "C-160", 0 },
  { "C162", "", "Cessna", "162 Skycatcher", 2 },
  { "C17", "", "Boeing", "C-17 Globemaster III", 170 },
  { "C170", "", "Cessna", "170", 4 },
  { "C172", "", "Cessna", "172", 4 },
  { "C175", "", "Cessna", "175", 4 },
  { "C177", "", "Cessna", "177 Cardinal", 4 },
  { "C180", "", "Cessna", "180 Skywagon", 4 },
  { "C182", "", "Cessna", "182", 4 },
  { "C185", "", "Cessna", "185 Skywagon", 6 },
  { "C188", "", "Cessna", "188", 1 },
  { "C195", "", "Cessna", "195", 5 },
  { "C206", "", "Cessna", "206", 6 },
  { "C207", "", "Cessna", "207 Stationair 7", 7 },
  { "C208", "", "Cessna", "208 Caravan", 12 },
  { "C208A", "", "Cessna", "208 Caravan Amphibian", 12 },
  { "C210", "", "Cessna", "210 Centurion", 6 },
  { "C212", "", "CASA", "212 Aviocar", 26 },
  { "C240", "", "Cessna", "TTx T240", 4 },
  { "C25A", "", "Cessna", "CJ2", 8 },
  { "C25B", "", "Cessna", "CJ3", 8 },
  { "C25C", "", "Cessna", "CJ4", 9 },
  { "C27J", "", "Leonardo", "C-27J Spartan", 60 },
  { "C295", "", "Airbus Military", "C-295", 71 },
  { "C303", "", "Cessna", "303 Crusader", 6 },
  { "C30J", "", "Lockheed Martin", "C-130J Hercules", 92 },
  { "C310", "", "Cessna", "310", 6 },
  { "C320", "", "Cessna", "320 Skyknight", 6 },
  { "C335", "", "Cessna", "335", 6 },
  { "C337", "", "Cessna", "337 Skymaster", 6 },
  { "C340", "", "Cessna", "340", 6 },
  { "C350", "", "Cessna", "350 Corvalis", 4 },
  { "C400", "", "Cessna", "400 Corvalis TT", 4 },
  { "C402", "", "Cessna", "401/402", 10 },
  { "C404", "", "Cessna", "404 Titan", 10 },
  { "C408", "", "Cessna", "408 SkyCourier", 19 },
  { "C414", "", "Cessna", "414 Chancellor", 8 },
  { "C421", "", "Cessna", "421 Golden Eagle", 8 },
  { "C425", "", "Cessna", "425 Corsair", 7 },
  { "C441", "", "Cessna", "441 Conquest", 9 },
  { "C5", "", "Lockheed Martin", "C-5 Galaxy", 345 },
  { "C500", "", "Cessna", "Citation I", 7 },
  { "C501", "", "Cessna", "Citation I/SP", 7 },
  { "C510", "", "Cessna", "Citation Mustang", 5 },
  { "C525", "", "Cessna", "CitationJet CJ1", 6 },
  { "C526", "", "Cessna", "526 CitationJet", 2 },
  { "C550", "", "Cessna", "Citation II/Bravo", 9 },
  { "C551", "", "Cessna", "Citation II/SP", 9 },
  { "C55B", "", "Cessna", "Citation Bravo", 9 },
  { "C560", "", "Cessna", "Citation V/Ultra/Encore", 9 },
  { "C56X", "", "Cessna", "Citation Excel/XLS", 9 },
  { "C5M", "", "Lockheed Martin", "C-5M Super Galaxy", 345 },
  { "C650", "", "Cessna", "Citation III/VI/VII", 13 },
  { "C680", "", "Cessna", "Citation Sovereign", 12 },
  { "C68A", "", "Cessna", "Citation Latitude", 12 },
  { "C700", "", "Cessna", "Citation Longitude", 12 },
  { "C72R", "", "Cessna", "172RG Cutlass RG", 4 },
  { "C750", "", "Cessna", "Citation X", 12 },
  { "C77R", "", "Cessna", "177RG", 4 },
  { "C82R", "", "Cessna", "182RG", 4 },
  { "C919", "", "Comac", "C919", 174 },
  { "CH47", "", "Boeing", "CH-47 Chinook", 55 },
  { "CH7A", "", "Aeronca", "7AC", 2 },
  { "CH7B", "", "Bellanca", "7GCBC Citabria", 2 },
  { "CL30", "C30", "Bombardier", "Challenger 300", 9 },
  { "CL35", "C35", "Bombardier", "Challenger 350", 10 },
  { "CL41", "", "Canadair", "CT-114 Tutor", 1 },
  { "CL60", "CRJ", "Bombardier", "Challenger 600", 12 },
  { "CN35", "", "Airbus Military", "CN-235", 45 },
  { "COL3", "", "Lancair", "LC-40 Columbia 300", 4 },
  { "COL4", "", "Lancair", "LC-41 Columbia 400", 4 },
  { "COUR", "", "Helio", "U-10 Super Courier", 6 },
  { "CRJ1", "CR1", "Bombardier", "CRJ100", 50 },
  { "CRJ2", "CR2", "Bombardier", "CRJ200", 50 },
  { "CRJ7", "CR7", "Bombardier", "CRJ700", 78 },
  { "CRJ9", "CR9", "Bombardier", "CRJ900", 90 },
  { "CRJX", "CRK", "Bombardier", "CRJ1000", 104 },
  { "CRUZ", "", "CZAW", "SportCruiser", 2 },
  { "CVLP", "", "Convair", "CV-440 Metropolitan", 86 },
  { "CVLT", "CV5", "Convair", "CV-580/600/640", 60 },
  { "D228", "", "Dornier", "Do 228", 19 },
  { "D328", "", "Dornier", "Do 328-100", 33 },
  { "DA40", "", "Diamond", "DA40", 4 },
  { "DA42", "", "Diamond", "DA42 Twin Star", 4 },
  { "DA62", "", "Diamond", "DA62", 7 },
  { "DC10", "D11", "Douglas", "DC-10", 380 },
  { "DC3", "", "Douglas", "DC-3", 32 },
  { "DC3S", "", "Douglas", "Super DC-3", 28 },
  { "DC3T", "", "Basler", "BT-67 (Turbo DC-3)", 18 },
  { "DC6", "", "Douglas", "DC-6", 102 },
  { "DC85", "D8T", "Douglas", "DC-8-50", 189 },
  { "DC86", "D8L", "Douglas", "DC-8-62", 189 },
  { "DC87", "D8Q", "Douglas", "DC-8-72", 189 },
  { "DC91", "D91", "Douglas", "DC-9-10", 109 },
  { "DC92", "D92", "Douglas", "DC-9-20", 125 },
  { "DC93", "D93", "Douglas", "DC-9-30", 135 },
  { "DC94", "D94", "Douglas", "DC-9-40", 135 },
  { "DC95", "D95", "Douglas", "DC-9-50", 139 },
  { "DH2T", "", "de Havilland Canada", "DHC-2T Turbo Beaver", 7 },
  { "DH3T", "", "de Havilland Canada", "DHC-3T Turbo Otter", 11 },
  { "DH8A", "DH1", "De Havilland Canada", "Dash 8-100", 39 },
  { "DH8B", "DH2", "De Havilland Canada", "Dash 8-200", 40 },
  { "DH8C", "DH3", "De Havilland Canada", "Dash 8-300", 56 },
  { "DH8D", "DH4", "De Havilland Canada", "Dash 8 Q400", 90 },
  { "DHC2", "", "de Havilland Canada", "DHC-2 Beaver", 7 },
  { "DHC3", "", "de Havilland Canada", "DHC-3 Otter", 11 },
  { "DHC5", "DHC", "De Havilland Canada", "DHC-5 Buffalo", 0 },
  { "DHC6", "", "de Havilland Canada", "DHC-6 Twin Otter", 19 },
  { "DHC7", "DH7", "De Havilland Canada", "DHC-7 Dash 7", 54 },
  { "DV20", "", "Diamond", "DA20 Katana", 2 },
  { "E110", "", "Embraer", "EMB-110 Bandeirante", 19 },
  { "E120", "", "Embraer", "EMB-120 Brasilia", 30 },
  { "E135", "", "Embraer", "ERJ 135", 37 },
  { "E140", "", "Embraer", "ERJ 140", 44 },
  { "E145", "", "Embraer", "ERJ 145", 50 },
  { "E170", "E70", "Embraer", "E170", 78 },
  { "E175", "E75", "Embraer", "E175", 88 },
  { "E190", "E90", "Embraer", "E190", 114 },
  { "E195", "E95", "Embraer", "E195", 132 },
  { "E290", "", "Embraer", "E190-E2", 120 },
  { "E295", "", "Embraer", "E195-E2", 146 },
  { "E35L", "ER3", "Embraer", "Legacy 600/650", 16 },
  { "E3TF", "", "Boeing", "E-3 Sentry AWACS", 19 },
  { "E45X", "", "Embraer", "ERJ 145XR", 50 },
  { "E50P", "", "Embraer", "Phenom 100", 6 },
  { "E545", "", "Embraer", "Legacy 450 / Praetor 500", 9 },
  { "E550", "", "Embraer", "Legacy 500 / Praetor 600", 12 },
  { "E55P", "", "Embraer", "Phenom 300", 9 },
  { "E7", "", "Boeing", "E-7 Wedgetail", 12 },
  { "E75L", "E75", "Embraer", "E175 (long wing)", 88 },
  { "E75S", "E75", "Embraer", "E175 (short wing)", 88 },
  { "EA50", "", "Eclipse", "Eclipse 500", 5 },
  { "EC35", "", "Airbus Helicopters", "H135/EC135", 7 },
  { "EC55", "", "Airbus Helicopters", "H155/EC155", 13 },
  { "ERCO", "", "ERCO", "Ercoupe 415", 2 },
  { "EUFI", "", "Eurofighter", "Typhoon", 2 },
  { "EVOT", "", "Lancair", "Evolution Turbine", 4 },
  { "F100", "100", "Fokker", "100", 109 },
  { "F16", "", "General Dynamics", "F-16 Fighting Falcon", 2 },
  { "F18", "", "McDonnell Douglas/Boeing", "F/A-18 Hornet", 2 },
  { "F22", "", "Lockheed Martin", "F-22 Raptor", 1 },
  { "F27", "F27", "Fokker", "F27 Friendship", 56 },
  { "F28", "F21", "Fokker", "F28 Fellowship", 85 },
  { "F2TH", "", "Dassault", "Falcon 2000", 12 },
  { "F2TP", "", "Dassault", "Falcon 2000S/LXS", 12 },
  { "F35", "", "Lockheed Martin", "F-35 Lightning II", 2 },
  { "F406", "", "Reims/Cessna", "F406 Caravan II", 9 },
  { "F50", "", "Fokker", "50", 62 },
  { "F70", "F70", "Fokker", "70", 85 },
  { "F900", "", "Dassault", "Falcon 900", 16 },
  { "FA10", "", "Dassault", "Falcon 10", 8 },
  { "FA20", "", "Dassault", "Falcon 20", 12 },
  { "FA50", "", "Dassault", "Falcon 50", 9 },
  { "FA6X", "", "Dassault", "Falcon 6X", 16 },
  { "FA7X", "", "Dassault", "Falcon 7X", 16 },
  { "FA8X", "", "Dassault", "Falcon 8X", 19 },
  { "FDCT", "", "Flight Design", "CT", 2 },
  { "G150", "", "Gulfstream", "G150", 8 },
  { "G164", "", "Grumman American", "G-164", 1 },
  { "G280", "", "Gulfstream", "G280", 10 },
  { "GA5C", "", "Gulfstream", "G500", 19 },
  { "GA6C", "", "Gulfstream", "G600", 19 },
  { "GA7", "", "Gulfstream American", "GA-7", 6 },
  { "GA7C", "", "Gulfstream", "G700", 19 },
  { "GALX", "", "IAI/Gulfstream", "1126 Galaxy/G200", 10 },
  { "GC1", "", "Globe", "GC-1 Swift", 2 },
  { "GL5T", "", "Bombardier", "Global 5000", 17 },
  { "GL7T", "", "Bombardier", "Global 7500", 19 },
  { "GLEX", "", "Bombardier", "Global Express", 17 },
  { "GLF2", "", "Gulfstream", "GII", 19 },
  { "GLF3", "", "Gulfstream", "GIII", 19 },
  { "GLF4", "", "Gulfstream", "GIV", 19 },
  { "GLF5", "", "Gulfstream", "GV", 19 },
  { "GLF6", "", "Gulfstream", "G650/G650ER", 19 },
  { "H160", "", "Airbus Helicopters", "H160", 12 },
  { "H25A", "", "Hawker Siddeley", "HS-125 (early)", 8 },
  { "H25B", "", "BAe/Hawker", "HS-125/800", 8 },
  { "H25C", "", "BAe/Raytheon", "HS-125-1000", 8 },
  { "HA4T", "", "Hawker", "4000", 9 },
  { "HAWK", "", "BAe", "T-45 Goshawk", 1 },
  { "HDJT", "", "Honda Aircraft", "HondaJet HA-420", 5 },
  { "HUSK", "", "Aviat", "Husky", 2 },
  { "IL18", "IL8", "Ilyushin", "Il-18", 120 },
  { "IL62", "IL6", "Ilyushin", "Il-62", 192 },
  { "IL76", "IL7", "Ilyushin", "Il-76", 0 },
  { "IL86", "ILW", "Ilyushin", "Il-86", 350 },
  { "IL96", "I93", "Ilyushin", "Il-96", 300 },
  { "J328", "", "Fairchild Dornier", "328JET", 33 },
  { "JS31", "", "British Aerospace", "Jetstream 31", 19 },
  { "JS32", "", "BAe", "Jetstream 32", 19 },
  { "JS41", "", "British Aerospace", "Jetstream 41", 30 },
  { "K35R", "K35", "Boeing", "KC-135 Stratotanker", 0 },
  { "KA32", "", "Kamov", "Ka-32", 16 },
  { "KC10", "", "McDonnell Douglas", "KC-10 Extender", 75 },
  { "KC46", "", "Boeing", "KC-46 Pegasus", 65 },
  { "KODI", "", "Quest", "Kodiak 100", 10 },
  { "L101", "L10", "Lockheed", "L-1011 TriStar", 400 },
  { "L188", "LOE", "Lockheed", "L-188 Electra", 98 },
  { "L29B", "", "Lockheed", "JetStar 2/731", 10 },
  { "L410", "L4T", "LET", "L-410", 19 },
  { "L5", "", "Stinson", "L-5 Sentinel", 2 },
  { "L8", "", "Luscombe", "8", 2 },
  { "LA4", "", "Lake", "LA-4", 4 },
  { "LA8", "", "Lake Aircraft", "LA-8", 6 },
  { "LJ23", "", "Learjet", "23", 6 },
  { "LJ24", "", "Learjet", "24", 6 },
  { "LJ25", "", "Learjet", "25", 8 },
  { "LJ31", "", "Learjet", "31", 8 },
  { "LJ40", "", "Learjet", "40", 7 },
  { "LJ45", "", "Learjet", "45", 8 },
  { "LJ55", "", "Learjet", "55", 10 },
  { "LJ60", "", "Learjet", "60", 8 },
  { "LJ70", "", "Learjet", "70", 9 },
  { "LJ75", "", "Learjet", "75", 9 },
  { "LNC4", "", "Lancair", "IV", 4 },
  { "LNP4", "", "Lancair", "PropJet IV", 4 },
  { "LR35", "", "Learjet", "35", 8 },
  { "LR45", "", "Learjet", "45", 8 },
  { "LR60", "", "Learjet", "60", 8 },
  { "M20P", "", "Mooney", "M20J", 4 },
  { "M20T", "", "Mooney", "M20K/M20M", 4 },
  { "M28", "", "PZL Mielec", "M28 Skytruck", 19 },
  { "M5", "", "Maule", "M-5", 4 },
  { "M600", "", "Piper", "M600", 6 },
  { "MD11", "M11", "McDonnell Douglas", "MD-11", 410 },
  { "MD81", "M81", "McDonnell Douglas", "MD-81", 155 },
  { "MD82", "M82", "McDonnell Douglas", "MD-82", 165 },
  { "MD83", "M83", "McDonnell Douglas", "MD-83", 165 },
  { "MD87", "M87", "McDonnell Douglas", "MD-87", 130 },
  { "MD88", "M88", "McDonnell Douglas", "MD-88", 165 },
  { "MD90", "M90", "McDonnell Douglas", "MD-90", 172 },
  { "MI8", "", "Mil", "Mi-8/17 Hip", 36 },
  { "MRJ9", "M90", "Mitsubishi", "SpaceJet M90", 92 },
  { "MU2", "", "Mitsubishi", "MU-2", 10 },
  { "MU30", "", "Mitsubishi", "MU-300 Diamond", 8 },
  { "NAVI", "", "North American", "Navion", 4 },
  { "P180", "P18", "Piaggio", "P.180 Avanti", 9 },
  { "P206", "", "Cessna", "P206 Pressurized Stationair", 6 },
  { "P210", "", "Cessna", "P210", 4 },
  { "P28A", "", "Piper", "PA-28 Archer", 4 },
  { "P28B", "", "Piper", "Turbo Dakota", 4 },
  { "P28R", "", "Piper", "PA-28R Arrow", 4 },
  { "P28T", "", "Piper", "PA-28T Arrow IV", 4 },
  { "P3", "", "Lockheed", "P-3 Orion", 11 },
  { "P32R", "", "Piper", "PA-32R Lance/Saratoga", 6 },
  { "P32T", "", "Piper", "PA-32T Turbo Lance II", 6 },
  { "P46T", "", "Piper", "Malibu Meridian", 6 },
  { "P51", "", "North American", "P-51 Mustang", 1 },
  { "P68", "", "Vulcanair", "P68", 6 },
  { "P750", "", "Pacific Aerospace", "P-750 XSTOL", 10 },
  { "P8", "", "Boeing", "P-8 Poseidon", 11 },
  { "PA11", "", "Piper", "PA-11 Cub Special", 2 },
  { "PA12", "", "Piper", "PA-12 Super Cruiser", 3 },
  { "PA16", "", "Piper", "PA-16 Clipper", 4 },
  { "PA18", "", "Piper", "PA-18 Super Cub", 2 },
  { "PA20", "", "Piper", "PA-20 Pacer", 4 },
  { "PA22", "", "Piper", "PA-22 Tri-Pacer", 4 },
  { "PA23", "", "Piper", "PA-23 Apache", 6 },
  { "PA24", "", "Piper", "PA-24 Comanche", 4 },
  { "PA25", "", "Piper", "PA-25 Pawnee", 1 },
  { "PA27", "", "Piper", "PA-27 Aztec", 6 },
  { "PA30", "", "Piper", "PA-30 Twin Comanche", 4 },
  { "PA31", "", "Piper", "Navajo/Chieftain", 9 },
  { "PA32", "", "Piper", "PA-32 Cherokee Six", 6 },
  { "PA34", "", "Piper", "Seneca", 6 },
  { "PA36", "", "Piper", "PA-36 Pawnee Brave", 1 },
  { "PA38", "", "Piper", "PA-38 Tomahawk", 2 },
  { "PA44", "", "Piper", "Seminole", 4 },
  { "PA46", "", "Piper", "Malibu/Mirage/Meridian", 6 },
  { "PAT4", "", "Piper", "T-1040", 37 },
  { "PAY1", "", "Piper", "Cheyenne I", 6 },
  { "PAY2", "", "Piper", "Cheyenne II", 9 },
  { "PAY3", "", "Piper", "PA-42-720 Cheyenne III", 9 },
  { "PAY4", "", "Piper", "Cheyenne 400LS", 9 },
  { "PC12", "", "Pilatus", "PC-12", 9 },
  { "PC24", "", "Pilatus", "PC-24", 10 },
  { "PRM1", "", "Raytheon", "Premier I", 6 },
  { "R22", "", "Robinson", "R22", 2 },
  { "R44", "", "Robinson", "R44", 4 },
  { "R66", "", "Robinson", "R66", 5 },
  { "R721", "", "Boeing", "727-100RE Super 27", 131 },
  { "R722", "", "Boeing", "727-200RE Super 27", 189 },
  { "RJ1H", "AR1", "Avro", "RJ100", 116 },
  { "RJ70", "AR7", "Avro", "RJ70", 82 },
  { "RJ85", "AR8", "Avro", "RJ85", 100 },
  { "RV12", "", "Van's Aircraft", "RV-12", 2 },
  { "S108", "", "Stinson", "108 Voyager", 4 },
  { "S22T", "", "Cirrus", "SR22 Turbo", 4 },
  { "S76", "", "Sikorsky", "S-76", 13 },
  { "S92", "", "Sikorsky", "S-92", 19 },
  { "SB20", "S20", "Saab", "2000", 58 },
  { "SBR1", "", "Rockwell", "Sabre 40/60", 8 },
  { "SBR2", "", "Rockwell", "Sabre 75", 8 },
  { "SC7", "SHS", "Shorts", "SC-7 Skyvan", 19 },
  { "SF34", "SF3", "Saab", "340B", 36 },
  { "SF50", "", "Cirrus", "Vision Jet SF50", 7 },
  { "SH33", "SH3", "Shorts", "SD-330", 36 },
  { "SH36", "SH6", "Shorts", "SD-360", 40 },
  { "SR20", "", "Cirrus", "SR20", 4 },
  { "SR22", "", "Cirrus", "SR22", 5 },
  { "SU95", "SU9", "Sukhoi", "Superjet 100", 108 },
  { "SW3", "", "Fairchild Swearingen", "SA-226", 11 },
  { "SW4", "SW4", "Swearingen", "Metroliner", 19 },
  { "T134", "TU3", "Tupolev", "Tu-134", 84 },
  { "T154", "T54", "Tupolev", "Tu-154", 180 },
  { "T204", "T20", "Tupolev", "Tu-204/214", 210 },
  { "T206", "", "Cessna", "T206 Turbo Stationair", 6 },
  { "T210", "", "Cessna", "T210 Turbo Centurion", 6 },
  { "T28", "", "North American", "T-28 Trojan", 2 },
  { "T34P", "", "Beech", "T-34/45 Mentor", 2 },
  { "T38", "", "Northrop", "T-38 Talon", 2 },
  { "T6", "", "North American", "T-6 Texan", 2 },
  { "TAYB", "", "Taylorcraft", "BC", 2 },
  { "TB20", "", "Socata", "TB-20 Trinidad", 4 },
  { "TBM7", "", "Daher", "TBM 700", 6 },
  { "TBM8", "", "Daher", "TBM 850", 6 },
  { "TBM9", "", "Daher", "TBM 900", 6 },
  { "TEX2", "", "Raytheon", "Texan II", 2 },
  { "TOBA", "", "Socata", "TB-10 Tobago", 4 },
  { "U206", "", "Cessna", "U206 Stationair", 6 },
  { "UH60", "", "Sikorsky", "UH-60 Black Hawk", 14 },
  { "V22", "", "Bell-Boeing", "V-22 Osprey", 24 },
  { "WW24", "WWP", "IAI", "1124 Westwind", 10 },
  { "Y12", "YN2", "Harbin", "Y-12", 19 },
  { "YK40", "YK4", "Yakovlev", "Yak-40", 32 },
  { "YK42", "YK2", "Yakovlev", "Yak-42", 120 },
  { "YS11", "", "NAMC", "YS-11", 64 },
};

static const size_t kTypeInfoCount = sizeof(kTypeInfo) / sizeof(kTypeInfo[0]);

// --- Lookup helpers ---

// Binary search by ICAO code. Returns pointer to match or nullptr.
// Table MUST be sorted by ICAO (case-insensitive) with unique keys.
static const AircraftTypeInfo* findByIcao(const char* icao) {
  if (!icao || !*icao) return nullptr;
  int lo = 0, hi = (int)kTypeInfoCount - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    int cmp = strcasecmp(icao, kTypeInfo[mid].icao);
    if (cmp == 0) return &kTypeInfo[mid];
    if (cmp < 0) hi = mid - 1;
    else lo = mid + 1;
  }
  return nullptr;
}

// ICAO-only lookup. The API's "t" field is always an ICAO designator, so an
// IATA fallback here could silently match a different aircraft's IATA code.
static const AircraftTypeInfo* aircraftLookup(const char* code) {
  return findByIcao(code);
}

// Family-prefix heuristics for seat count when no exact table match exists.
// Only covers broad aircraft families that can't be enumerated individually.
static bool aircraftSeatMaxHeuristic(const char* icao, uint16_t& maxOut) {
  if (!icao || !*icao) return false;
  // Airbus narrowbody families
  if (strncasecmp(icao, "A31", 3) == 0 || strncasecmp(icao, "A32", 3) == 0) { maxOut = 244; return true; }
  // Boeing families
  if (strncasecmp(icao, "B70", 3) == 0) { maxOut = 189; return true; }  // 707
  if (strncasecmp(icao, "B72", 3) == 0) { maxOut = 189; return true; }  // 727
  if (strncasecmp(icao, "B73", 3) == 0) { maxOut = 230; return true; }  // 737
  if (strncasecmp(icao, "B74", 3) == 0) { maxOut = 524; return true; }  // 747
  if (strncasecmp(icao, "B75", 3) == 0) { maxOut = 295; return true; }  // 757
  if (strncasecmp(icao, "B76", 3) == 0) { maxOut = 304; return true; }  // 767
  if (strncasecmp(icao, "B77", 3) == 0) { maxOut = 451; return true; }  // 777
  if (strncasecmp(icao, "B78", 3) == 0) { maxOut = 330; return true; }  // 787
  // Embraer E-Jet families
  if (strncasecmp(icao, "E17", 3) == 0 || strncasecmp(icao, "E19", 3) == 0) { maxOut = 146; return true; }
  if (strncasecmp(icao, "E29", 3) == 0 || strncasecmp(icao, "E75", 3) == 0) { maxOut = 146; return true; }
  // Bombardier CRJ
  if (strncasecmp(icao, "CRJ", 3) == 0) { maxOut = 104; return true; }
  // ATR
  if (strncasecmp(icao, "AT4", 3) == 0 || strncasecmp(icao, "AT7", 3) == 0) { maxOut = 78; return true; }
  // De Havilland Dash 8
  if (strncasecmp(icao, "DH8", 3) == 0) { maxOut = 90; return true; }
  if (strncasecmp(icao, "DH2", 3) == 0) { maxOut = 7; return true; }
  // Learjet family
  if (strncasecmp(icao, "LJ", 2) == 0 || strncasecmp(icao, "LR", 2) == 0) { maxOut = 9; return true; }
  // Cessna singles (broad prefix)
  if (strncasecmp(icao, "C15", 3) == 0) { maxOut = 2; return true; }
  if (strncasecmp(icao, "C17", 3) == 0 || strncasecmp(icao, "C18", 3) == 0) { maxOut = 4; return true; }
  // Piper PA-28 family
  if (strncasecmp(icao, "PA28", 4) == 0 || strncasecmp(icao, "P28", 3) == 0) { maxOut = 4; return true; }
  // Beechcraft King Air
  if (strncasecmp(icao, "BE9", 3) == 0) { maxOut = 9; return true; }
  // BAe 146 family
  if (strncasecmp(icao, "BA1", 3) == 0) { maxOut = 116; return true; }
  // Douglas DC-9 family
  if (strncasecmp(icao, "DC9", 3) == 0) { maxOut = 139; return true; }
  // McDonnell Douglas MD-80/90
  if (strncasecmp(icao, "MD8", 3) == 0 || strncasecmp(icao, "MD9", 3) == 0) { maxOut = 172; return true; }
  // COMAC C919
  if (strncasecmp(icao, "C91", 3) == 0) { maxOut = 174; return true; }
  // Mitsubishi MU-2
  if (strncasecmp(icao, "MU2", 3) == 0) { maxOut = 10; return true; }
  // PZL M28
  if (strncasecmp(icao, "M28", 3) == 0) { maxOut = 19; return true; }
  // TIS-B pseudo-types
  if (strncasecmp(icao, "TISB", 4) == 0) { maxOut = 6; return true; }
  return false;
}

// --- Public API ---

// Return max seat count for a type code. Returns true if found.
inline bool aircraftSeatMax(const char* code, uint16_t& maxOut) {
  if (!code || !*code) return false;
  const AircraftTypeInfo* info = aircraftLookup(code);
  if (info) { maxOut = info->maxSeats; return true; }
  return aircraftSeatMaxHeuristic(code, maxOut);
}

// Arduino String wrapper
inline bool aircraftSeatMax(const String& rawCode, uint16_t& maxOut) {
  if (rawCode.length() == 0) return false;
  String code = rawCode;
  code.trim();
  return aircraftSeatMax(code.c_str(), maxOut);
}

// Build "Manufacturer Model" friendly name into caller buffer.
// Returns true if found. Buffer is null-terminated.
inline bool aircraftFriendlyNameBuf(const char* code, char* buf, size_t bufLen) {
  if (!code || !*code || !buf || bufLen == 0) { if (buf && bufLen) buf[0] = '\0'; return false; }
  const AircraftTypeInfo* info = aircraftLookup(code);
  if (!info) { buf[0] = '\0'; return false; }
  const char* manuf = info->manufacturer ? info->manufacturer : "";
  const char* model = info->model ? info->model : "";
  // Avoid duplication if model already starts with manufacturer
  if (*manuf && strncasecmp(model, manuf, strlen(manuf)) == 0) {
    snprintf(buf, bufLen, "%s", model);
  } else if (*manuf) {
    snprintf(buf, bufLen, "%s %s", manuf, model);
  } else {
    snprintf(buf, bufLen, "%s", model);
  }
  return true;
}

// Legacy Arduino String wrapper (for backward compatibility)
inline String aircraftFriendlyName(const String& rawCode) {
  if (rawCode.length() == 0) return String("");
  String code = rawCode;
  code.trim();
  char buf[64];
  if (aircraftFriendlyNameBuf(code.c_str(), buf, sizeof(buf))) {
    return String(buf);
  }
  return String("");
}

// Compose display string "CODE FriendlyName" with graceful fallbacks
inline String aircraftDisplayType(const String& rawCode) {
  String code = rawCode;
  code.trim();
  String name = aircraftFriendlyName(code);
  if (!name.length()) return code;
  if (!code.length()) return name;
  return code + " " + name;
}
