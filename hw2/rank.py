from subprocess import check_output
from collections import Counter

from pprint import pprint


def extractBid(playerIDs):
    res = {}
    for id in playerIDs:
        out = check_output(["./player", str(id)]).decode()
        res[id] = [int(i.split(" ")[1]) for i in out.split("\n") if i]
    return res


def rankByScore(score):
    rank = [0] * len(score)
    for ind, i in enumerate(score):
        nBigger = 0
        for j in score:
            if i < j:
                nBigger += 1
        rank[ind] = nBigger + 1
    return rank


def main():
    playerIDs = [i + 1 for i in range(8)]
    n = len(playerIDs)
    nRound = 10

    bids = extractBid(playerIDs)
    winners = [max((bids[id][i], id) for id in playerIDs) for i in range(nRound)]

    pos = {id: i for i, id in enumerate(playerIDs)}
    totalScore = [0] * n
    for _, id in winners:
        totalScore[pos[id]] += 1

    rank = rankByScore(totalScore)

    print("Raw data:")
    pprint(bids)
    print()

    print("player id:\t", playerIDs)
    print("player score:\t", totalScore)
    print("player rank:\t", rank)


if __name__ == "__main__":
    main()
