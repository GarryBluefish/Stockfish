/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2009 Marco Costalba

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


////
//// Includes
////

#include <cassert>
#include <sstream>
#include <map>

#include "material.h"

using namespace std;


////
//// Local definitions
////

namespace {

  // Values modified by Joona Kiiski
  const Value MidgameLimit = Value(15581);
  const Value EndgameLimit = Value(3998);

  // Polynomial material balance parameters
  const Value RedundantQueenPenalty = Value(320);
  const Value RedundantRookPenalty  = Value(554);
  const int LinearCoefficients[6]   = { 1617, -162, -1172, -190, 105, 26 };

  const int QuadraticCoefficientsSameColor[][6] = {
  { 7, 7, 7, 7, 7, 7 }, { 39, 2, 7, 7, 7, 7 }, { 35, 271, -4, 7, 7, 7 },
  { 7, 25, 4, 7, 7, 7 }, { -27, -2, 46, 100, 56, 7 }, { 58, 29, 83, 148, -3, -25 } };

  const int QuadraticCoefficientsOppositeColor[][6] = {
  { 41, 41, 41, 41, 41, 41 }, { 37, 41, 41, 41, 41, 41 }, { 10, 62, 41, 41, 41, 41 },
  { 57, 64, 39, 41, 41, 41 }, { 50, 40, 23, -22, 41, 41 }, { 106, 101, 3, 151, 171, 41 } };

  // Named endgame evaluation and scaling functions, these
  // are accessed direcly and not through the function maps.
  EvaluationFunction<KmmKm> EvaluateKmmKm(WHITE);
  EvaluationFunction<KXK>   EvaluateKXK(WHITE), EvaluateKKX(BLACK);
  ScalingFunction<KBPsK>    ScaleKBPsK(WHITE),  ScaleKKBPs(BLACK);
  ScalingFunction<KQKRPs>   ScaleKQKRPs(WHITE), ScaleKRPsKQ(BLACK);
  ScalingFunction<KPsK>     ScaleKPsK(WHITE),   ScaleKKPs(BLACK);
  ScalingFunction<KPKP>     ScaleKPKPw(WHITE),  ScaleKPKPb(BLACK);

  typedef EndgameEvaluationFunctionBase EF;
  typedef EndgameScalingFunctionBase SF;
}


////
//// Classes
////

/// EndgameFunctions class stores endgame evaluation and scaling functions
/// in two std::map. Because STL library is not guaranteed to be thread
/// safe even for read access, the maps, although with identical content,
/// are replicated for each thread. This is faster then using locks.

class EndgameFunctions {
public:
  EndgameFunctions();
  ~EndgameFunctions();
  template<class T> T* get(Key key) const;

private:
  template<class T> void add(const string& keyCode);

  static Key buildKey(const string& keyCode);
  static const string swapColors(const string& keyCode);

  // Here we store two maps, for evaluate and scaling functions
  pair<map<Key, EF*>, map<Key, SF*> > maps;

  // Maps accessing functions returning const and non-const references
  template<typename T> const map<Key, T*>& get() const { return maps.first; }
  template<typename T> map<Key, T*>& get() { return maps.first; }
};

// Explicit specializations of a member function shall be declared in
// the namespace of which the class template is a member.
template<> const map<Key, SF*>&
EndgameFunctions::get<SF>() const { return maps.second; }

template<> map<Key, SF*>&
EndgameFunctions::get<SF>() { return maps.second; }


////
//// Functions
////

/// MaterialInfoTable c'tor and d'tor, called once by each thread

MaterialInfoTable::MaterialInfoTable(unsigned int numOfEntries) {

  size = numOfEntries;
  entries = new MaterialInfo[size];
  funcs = new EndgameFunctions();

  if (!entries || !funcs)
  {
      cerr << "Failed to allocate " << numOfEntries * sizeof(MaterialInfo)
           << " bytes for material hash table." << endl;
      Application::exit_with_failure();
  }
}

MaterialInfoTable::~MaterialInfoTable() {

  delete funcs;
  delete [] entries;
}


/// MaterialInfoTable::game_phase() calculate the phase given the current
/// position. Because the phase is strictly a function of the material, it
/// is stored in MaterialInfo.

Phase MaterialInfoTable::game_phase(const Position& pos) {

  Value npm = pos.non_pawn_material(WHITE) + pos.non_pawn_material(BLACK);

  if (npm >= MidgameLimit)
      return PHASE_MIDGAME;
  else if (npm <= EndgameLimit)
      return PHASE_ENDGAME;

  return Phase(((npm - EndgameLimit) * 128) / (MidgameLimit - EndgameLimit));
}

/// MaterialInfoTable::get_material_info() takes a position object as input,
/// computes or looks up a MaterialInfo object, and returns a pointer to it.
/// If the material configuration is not already present in the table, it
/// is stored there, so we don't have to recompute everything when the
/// same material configuration occurs again.

MaterialInfo* MaterialInfoTable::get_material_info(const Position& pos) {

  Key key = pos.get_material_key();
  int index = key & (size - 1);
  MaterialInfo* mi = entries + index;

  // If mi->key matches the position's material hash key, it means that we
  // have analysed this material configuration before, and we can simply
  // return the information we found the last time instead of recomputing it.
  if (mi->key == key)
      return mi;

  // Clear the MaterialInfo object, and set its key
  mi->clear();
  mi->key = key;

  // Calculate game phase
  mi->gamePhase = MaterialInfoTable::game_phase(pos);

  // Let's look if we have a specialized evaluation function for this
  // particular material configuration. First we look for a fixed
  // configuration one, then a generic one if previous search failed.
  if ((mi->evaluationFunction = funcs->get<EF>(key)) != NULL)
      return mi;

  else if (   pos.non_pawn_material(BLACK) == Value(0)
           && pos.piece_count(BLACK, PAWN) == 0
           && pos.non_pawn_material(WHITE) >= RookValueMidgame)
  {
      mi->evaluationFunction = &EvaluateKXK;
      return mi;
  }
  else if (   pos.non_pawn_material(WHITE) == Value(0)
           && pos.piece_count(WHITE, PAWN) == 0
           && pos.non_pawn_material(BLACK) >= RookValueMidgame)
  {
      mi->evaluationFunction = &EvaluateKKX;
      return mi;
  }
  else if (   pos.pieces(PAWN) == EmptyBoardBB
           && pos.pieces(ROOK) == EmptyBoardBB
           && pos.pieces(QUEEN) == EmptyBoardBB)
  {
      // Minor piece endgame with at least one minor piece per side and
      // no pawns. Note that the case KmmK is already handled by KXK.
      assert((pos.pieces(KNIGHT, WHITE) | pos.pieces(BISHOP, WHITE)));
      assert((pos.pieces(KNIGHT, BLACK) | pos.pieces(BISHOP, BLACK)));

      if (   pos.piece_count(WHITE, BISHOP) + pos.piece_count(WHITE, KNIGHT) <= 2
          && pos.piece_count(BLACK, BISHOP) + pos.piece_count(BLACK, KNIGHT) <= 2)
      {
          mi->evaluationFunction = &EvaluateKmmKm;
          return mi;
      }
  }

  // OK, we didn't find any special evaluation function for the current
  // material configuration. Is there a suitable scaling function?
  //
  // The code below is rather messy, and it could easily get worse later,
  // if we decide to add more special cases. We face problems when there
  // are several conflicting applicable scaling functions and we need to
  // decide which one to use.
  SF* sf;

  if ((sf = funcs->get<SF>(key)) != NULL)
  {
      mi->scalingFunction[sf->color()] = sf;
      return mi;
  }

  // Generic scaling functions that refer to more then one material
  // distribution. Should be probed after the specialized ones.
  // Note that these ones don't return after setting the function.
  if (   pos.non_pawn_material(WHITE) == BishopValueMidgame
      && pos.piece_count(WHITE, BISHOP) == 1
      && pos.piece_count(WHITE, PAWN) >= 1)
      mi->scalingFunction[WHITE] = &ScaleKBPsK;

  if (   pos.non_pawn_material(BLACK) == BishopValueMidgame
      && pos.piece_count(BLACK, BISHOP) == 1
      && pos.piece_count(BLACK, PAWN) >= 1)
      mi->scalingFunction[BLACK] = &ScaleKKBPs;

  if (   pos.piece_count(WHITE, PAWN) == 0
      && pos.non_pawn_material(WHITE) == QueenValueMidgame
      && pos.piece_count(WHITE, QUEEN) == 1
      && pos.piece_count(BLACK, ROOK) == 1
      && pos.piece_count(BLACK, PAWN) >= 1)
      mi->scalingFunction[WHITE] = &ScaleKQKRPs;

  else if (   pos.piece_count(BLACK, PAWN) == 0
           && pos.non_pawn_material(BLACK) == QueenValueMidgame
           && pos.piece_count(BLACK, QUEEN) == 1
           && pos.piece_count(WHITE, ROOK) == 1
           && pos.piece_count(WHITE, PAWN) >= 1)
      mi->scalingFunction[BLACK] = &ScaleKRPsKQ;

  if (pos.non_pawn_material(WHITE) + pos.non_pawn_material(BLACK) == Value(0))
  {
      if (pos.piece_count(BLACK, PAWN) == 0)
      {
          assert(pos.piece_count(WHITE, PAWN) >= 2);
          mi->scalingFunction[WHITE] = &ScaleKPsK;
      }
      else if (pos.piece_count(WHITE, PAWN) == 0)
      {
          assert(pos.piece_count(BLACK, PAWN) >= 2);
          mi->scalingFunction[BLACK] = &ScaleKKPs;
      }
      else if (pos.piece_count(WHITE, PAWN) == 1 && pos.piece_count(BLACK, PAWN) == 1)
      {
          // This is a special case because we set scaling functions
          // for both colors instead of only one.
          mi->scalingFunction[WHITE] = &ScaleKPKPw;
          mi->scalingFunction[BLACK] = &ScaleKPKPb;
      }
  }

  // Compute the space weight
  if (pos.non_pawn_material(WHITE) + pos.non_pawn_material(BLACK) >=
      2*QueenValueMidgame + 4*RookValueMidgame + 2*KnightValueMidgame)
  {
      int minorPieceCount =  pos.piece_count(WHITE, KNIGHT)
                           + pos.piece_count(BLACK, KNIGHT)
                           + pos.piece_count(WHITE, BISHOP)
                           + pos.piece_count(BLACK, BISHOP);

      mi->spaceWeight = minorPieceCount * minorPieceCount;
  }

  // Evaluate the material balance
  const int pieceCount[2][6] = { { pos.piece_count(WHITE, BISHOP) > 1, pos.piece_count(WHITE, PAWN), pos.piece_count(WHITE, KNIGHT),
                                   pos.piece_count(WHITE, BISHOP), pos.piece_count(WHITE, ROOK), pos.piece_count(WHITE, QUEEN) },
                                 { pos.piece_count(BLACK, BISHOP) > 1, pos.piece_count(BLACK, PAWN), pos.piece_count(BLACK, KNIGHT),
                                   pos.piece_count(BLACK, BISHOP), pos.piece_count(BLACK, ROOK), pos.piece_count(BLACK, QUEEN) } };
  Color c, them;
  int sign;
  int matValue = 0;

  for (c = WHITE, sign = 1; c <= BLACK; c++, sign = -sign)
  {
    // No pawns makes it difficult to win, even with a material advantage
    if (   pos.piece_count(c, PAWN) == 0
        && pos.non_pawn_material(c) - pos.non_pawn_material(opposite_color(c)) <= BishopValueMidgame)
    {
        if (   pos.non_pawn_material(c) == pos.non_pawn_material(opposite_color(c))
            || pos.non_pawn_material(c) < RookValueMidgame)
            mi->factor[c] = 0;
        else
        {
            switch (pos.piece_count(c, BISHOP)) {
            case 2:
                mi->factor[c] = 32;
                break;
            case 1:
                mi->factor[c] = 12;
                break;
            case 0:
                mi->factor[c] = 6;
                break;
            }
        }
    }

    // Redundancy of major pieces, formula based on Kaufman's paper
    // "The Evaluation of Material Imbalances in Chess"
    // http://mywebpages.comcast.net/danheisman/Articles/evaluation_of_material_imbalance.htm
    if (pieceCount[c][ROOK] >= 1)
        matValue -= sign * ((pieceCount[c][ROOK] - 1) * RedundantRookPenalty + pieceCount[c][QUEEN] * RedundantQueenPenalty);

    them = opposite_color(c);

    // Second-degree polynomial material imbalance by Tord Romstad
    //
    // We use NO_PIECE_TYPE as a place holder for the bishop pair "extended piece",
    // this allow us to be more flexible in defining bishop pair bonuses.
    for (int pt1 = NO_PIECE_TYPE; pt1 <= QUEEN; pt1++)
    {
        int c1 = sign * pieceCount[c][pt1];
        if (!c1)
            continue;

        matValue += c1 * LinearCoefficients[pt1];

        for (int pt2 = NO_PIECE_TYPE; pt2 <= pt1; pt2++)
        {
            matValue += c1 * pieceCount[c][pt2] * QuadraticCoefficientsSameColor[pt1][pt2];
            matValue += c1 * pieceCount[them][pt2] * QuadraticCoefficientsOppositeColor[pt1][pt2];
        }
    }
  }
  mi->value = int16_t(matValue / 16);
  return mi;
}


/// EndgameFunctions member definitions.

EndgameFunctions::EndgameFunctions() {

  add<EvaluationFunction<KNNK>  >("KNNK");
  add<EvaluationFunction<KPK>   >("KPK");
  add<EvaluationFunction<KBNK>  >("KBNK");
  add<EvaluationFunction<KRKP>  >("KRKP");
  add<EvaluationFunction<KRKB>  >("KRKB");
  add<EvaluationFunction<KRKN>  >("KRKN");
  add<EvaluationFunction<KQKR>  >("KQKR");
  add<EvaluationFunction<KBBKN> >("KBBKN");

  add<ScalingFunction<KNPK>    >("KNPK");
  add<ScalingFunction<KRPKR>   >("KRPKR");
  add<ScalingFunction<KBPKB>   >("KBPKB");
  add<ScalingFunction<KBPPKB>  >("KBPPKB");
  add<ScalingFunction<KBPKN>   >("KBPKN");
  add<ScalingFunction<KRPPKRP> >("KRPPKRP");
  add<ScalingFunction<KRPPKRP> >("KRPPKRP");
}

EndgameFunctions::~EndgameFunctions() {

    for (map<Key, EF*>::iterator it = maps.first.begin(); it != maps.first.end(); ++it)
        delete (*it).second;

    for (map<Key, SF*>::iterator it = maps.second.begin(); it != maps.second.end(); ++it)
        delete (*it).second;
}

Key EndgameFunctions::buildKey(const string& keyCode) {

    assert(keyCode.length() > 0 && keyCode[0] == 'K');
    assert(keyCode.length() < 8);

    stringstream s;
    bool upcase = false;

    // Build up a fen string with the given pieces, note that
    // the fen string could be of an illegal position.
    for (size_t i = 0; i < keyCode.length(); i++)
    {
        if (keyCode[i] == 'K')
            upcase = !upcase;

        s << char(upcase? toupper(keyCode[i]) : tolower(keyCode[i]));
    }
    s << 8 - keyCode.length() << "/8/8/8/8/8/8/8 w -";
    return Position(s.str()).get_material_key();
}

const string EndgameFunctions::swapColors(const string& keyCode) {

    // Build corresponding key for the opposite color: "KBPKN" -> "KNKBP"
    size_t idx = keyCode.find("K", 1);
    return keyCode.substr(idx) + keyCode.substr(0, idx);
}

template<class T>
void EndgameFunctions::add(const string& keyCode) {

  typedef typename T::Base F;

  get<F>().insert(pair<Key, F*>(buildKey(keyCode), new T(WHITE)));
  get<F>().insert(pair<Key, F*>(buildKey(swapColors(keyCode)), new T(BLACK)));
}

template<class T>
T* EndgameFunctions::get(Key key) const {

  typename map<Key, T*>::const_iterator it(get<T>().find(key));
  return (it != get<T>().end() ? it->second : NULL);
}
