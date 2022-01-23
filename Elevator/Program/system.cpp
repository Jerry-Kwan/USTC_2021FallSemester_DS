#include "system.h"

using namespace std;

int g_current_time = 0;
int g_capacity;
int g_max_time;
int g_elevator_num;
int g_total_floor; // if 3, that means it has floor0, floor1 and floor2
int g_id = 0;
int g_next_passenger_inter_time = 0;

int g_new_num = 0;
int g_get_in_num = 0;
int g_get_out_num = 0;
int g_give_up_num = 0;

/*
 * regardless of how many lifts, each floor has only one up button and one down button
 *  0: hasn't been pressed
 *  1: has been pressed but no lift "know"
 * -1: has been pressed and **only one** lift "know",
 *     but lift hasn't arrived (so -1 serves as a "source lock", avoid lots of lift go there together)
 * -2: has been pressed and lift has arrived
 */
int *g_call_up, *g_call_down;

WaitQueuePtr *g_wait_queue; // g_wait_queue[UP]: up, [DOWN]: down

ElevatorPtr g_elevator;

// Stack
// -------------------------------------------------------------------------
void InitStack(PassengerStack &s)
{
    s.base = (PassengerPtr *)malloc(STACK_INIT_SIZE * sizeof(PassengerPtr));
    if (!s.base)
    {
        cout << "overflow" << endl;
        exit(OVERFLOW);
    }
    s.top = s.base;
    s.stack_size = STACK_INIT_SIZE;
}

void DestroyStack(PassengerStack &s)
{
    PassengerPtr *p = s.base;
    while (p != s.top)
    {
        if (*p)
            free(*p); // free passenger in the elevator
        free(p);      // free passenger ptr in the elevator
        ++p;
    }
}

bool StackEmpty(PassengerStack &s)
{
    return s.base == s.top;
}

void Push(PassengerStack &s, PassengerPtr p)
{
    if (s.top - s.base >= s.stack_size)
    {
        s.base = (PassengerPtr *)realloc(s.base, (s.stack_size + STACK_INCREMENT) * sizeof(PassengerPtr));
        if (!s.base)
        {
            cout << "overflow" << endl;
            exit(OVERFLOW);
        }
        s.top = s.base + s.stack_size;
        s.stack_size += STACK_INCREMENT;
    }
    *s.top++ = p;
}

PassengerPtr Pop(PassengerStack &s)
{
    if (s.base == s.top)
    {
        cout << "stack empty" << endl;
        exit(STACK_EMPTY);
    }
    return *(--s.top);
}
// -------------------------------------------------------------------------

// Queue
// -------------------------------------------------------------------------
void InitQueue(WaitQueue &q)
{
    q.front = q.rear = (WaitQueueNodePtr)malloc(sizeof(WaitQueueNode));
    if (!q.front)
    {
        cout << "overflow" << endl;
        exit(OVERFLOW);
    }
    q.num = 0;
    q.front->data = NULL;
    q.front->next = NULL;
}

void DestroyQueue(WaitQueue &q)
{
    while (q.front)
    {
        q.rear = q.front->next;
        if (q.front->data)
            free(q.front->data); // free passenger in wait queue, it will not conflict with DestroyStack
        free(q.front);
        q.front = q.rear;
    }
}

bool QueueEmpty(WaitQueue &q)
{
    return q.front == q.rear;
}

void EnQueue(WaitQueue &q, PassengerPtr p)
{
    WaitQueueNodePtr t = (WaitQueueNodePtr)malloc(sizeof(WaitQueueNode));
    if (!t)
    {
        cout << "overflow" << endl;
        exit(OVERFLOW);
    }
    t->data = p;
    t->next = NULL;
    q.rear->next = t;
    q.rear = t;
    ++q.num;
}

PassengerPtr DeQueue(WaitQueue &q)
{
    if (q.front == q.rear)
    {
        cout << "queue empty" << endl;
        exit(QUEUE_EMPTY);
    }
    WaitQueueNodePtr t = q.front->next;
    PassengerPtr p = t->data;
    q.front->next = t->next;
    if (q.rear == t)
        q.rear = q.front;
    free(t);
    --q.num;
    return p;
}

int NumBefore(WaitQueue &q, WaitQueueNodePtr p)
{
    // assume that p won't be NULL
    int count = 0;
    WaitQueueNodePtr t = q.front;
    while (t->next != p)
    {
        ++count;
        t = t->next;
    }
    return count;
}

void CheckQueueForGiveUp(enum WaitQueueType type, int floor)
{
    /*
     * note:
     * it's impossible for the first person in queue to be going into lift right now,
     * because what I write is that if sb. can go into lift,
     * he will immediately DeQueue and Push to stack,
     * and wait for IN_OUT_TIME in stack
     * assume:
     * though someone's give-up time has decreased to <= 0,
     * he won't leave if he find that he is able to take the lift
     */
    WaitQueueNodePtr p = g_wait_queue[type][floor].front;
    while (p->next)
    {
        if (p->next->data->give_up_time > 0) // can't miss "> 0" because it can be < 0
        {
            p = p->next;
            p->data->give_up_time--;
        }
        else
        {
            int i;
            for (i = 0; i < g_elevator_num; ++i)
            {
                if (g_elevator[i].current_floor == floor &&
                    (g_elevator[i].move == OPENING || g_elevator[i].move == OPENED) &&
                    ((type == UP && g_elevator[i].state != GOING_DOWN) || (type == DOWN && g_elevator[i].state != GOING_UP)) &&
                    g_elevator[i].passenger_num + NumBefore(g_wait_queue[type][floor], p->next) < g_elevator[i].capacity)
                    break;
            }
            if (i < g_elevator_num)
                p = p->next;
            else
            {
                // should leave
                ++g_give_up_num;
                PassengerPtr pp = p->next->data;
                --g_wait_queue[type][floor].num;
                WaitQueueNodePtr temp = p->next;
                p->next = temp->next;
                if (temp == g_wait_queue[type][floor].rear)
                    g_wait_queue[type][floor].rear = p;
                Show(GIVE_UP, pp->id, pp->in_floor, pp->out_floor, -1, -1);
                free(pp);
                free(temp);
            }
        }
    }
}

void CheckGiveUp()
{
    for (int i = 0; i < g_total_floor; ++i)
    {
        CheckQueueForGiveUp(UP, i);
        CheckQueueForGiveUp(DOWN, i);
    }
}
// -------------------------------------------------------------------------

// Passenger
void NewPassenger()
{
    /*
     * 1. new a passenger, enqueue, press button and show
     * 2. prepare for the next passenger
     */
    PassengerPtr p = (PassengerPtr)malloc(sizeof(Passenger));
    if (!p)
    {
        cout << "overflow" << endl;
        exit(OVERFLOW);
    }
    ++g_new_num;
    p->id = g_id++;
    p->give_up_time = rand() % MAX_GIVE_UP_TIME + 300; // can't be 0
    p->in_floor = rand() % g_total_floor;
    p->out_floor = rand() % g_total_floor;
    while (p->in_floor == p->out_floor)
        p->out_floor = rand() % g_total_floor;
    if (p->in_floor < p->out_floor)
    {
        EnQueue(g_wait_queue[UP][p->in_floor], p);
        if (!g_call_up[p->in_floor])
            g_call_up[p->in_floor] = 1;
        // when door is closing, no need to press again, because what I write is that
        // the lift will check whether there is someone in queue "automatically"
        // also when door has been closed and passengers in the queue haven't pressed button,
        // they don't need to press again because lift will press it "automatically"
    }
    else
    {
        EnQueue(g_wait_queue[DOWN][p->in_floor], p);
        if (!g_call_down[p->in_floor])
            g_call_down[p->in_floor] = 1;
    }
    Show(NEW_PASSENGER, p->id, p->in_floor, p->out_floor, -1, -1);
    g_next_passenger_inter_time = rand() % MAX_INTER_TIME + 50; // if +1 maybe too small and door can only close when lift is full?
}

bool PassengerOut(int lift)
{
    // if sb. goes out, return true; else return false
    // only Pop one passenger
    if (!StackEmpty(g_elevator[lift].stack_array[g_elevator[lift].current_floor]))
    {
        // steps: pop -> change elevator attributes -> show -> delete sb. -> return true
        ++g_get_out_num;
        PassengerPtr pp = Pop(g_elevator[lift].stack_array[g_elevator[lift].current_floor]);
        int i;
        for (i = 0; i < g_elevator[lift].passenger_num; ++i)
            if (g_elevator[lift].passenger_id[i] == pp->id)
                break;
        for (int j = i + 1; j < g_elevator[lift].passenger_num; ++j)
            g_elevator[lift].passenger_id[j - 1] = g_elevator[lift].passenger_id[j];
        --g_elevator[lift].passenger_num;
        Show(PASSENGER_OUT, pp->id, pp->in_floor, pp->out_floor, lift, pp->out_floor);
        free(pp);
        return true;
    }
    return false;
}

bool PassengerIn(int lift)
{
    // only go in one passenger
    if (g_elevator[lift].passenger_num == g_elevator[lift].capacity)
        return false;
    switch (g_elevator[lift].state)
    {
    case GOING_UP:
    {
        if (!g_wait_queue[UP][g_elevator[lift].current_floor].num)
            return false;

        // steps: DeQueue, change Queue attributes, Push, change Elevator attributes,
        //        change global variables, show, return true
        // remember: Push to the stack of out_floor!!! Not in_floor!!!
        // don't forget to press button!!!!!!!!!!!!!!!!!!!!
        ++g_get_in_num;
        PassengerPtr pp = DeQueue(g_wait_queue[UP][g_elevator[lift].current_floor]);
        Push(g_elevator[lift].stack_array[pp->out_floor], pp);
        g_elevator[lift].passenger_id[g_elevator[lift].passenger_num++] = pp->id;
        g_elevator[lift].call_car[pp->out_floor] = 1;
        Show(PASSENGER_IN, pp->id, pp->in_floor, pp->out_floor, lift, pp->in_floor);
        return true;
    }

    case GOING_DOWN:
    {
        if (!g_wait_queue[DOWN][g_elevator[lift].current_floor].num)
            return false;

        ++g_get_in_num;
        PassengerPtr pp = DeQueue(g_wait_queue[DOWN][g_elevator[lift].current_floor]);
        Push(g_elevator[lift].stack_array[pp->out_floor], pp);
        g_elevator[lift].passenger_id[g_elevator[lift].passenger_num++] = pp->id;
        g_elevator[lift].call_car[pp->out_floor] = 1;
        Show(PASSENGER_IN, pp->id, pp->in_floor, pp->out_floor, lift, pp->in_floor);
        return true;
    }

    case IDLE:
        exit(UNKNOWN_ERROR);
    }
}
// -------------------------------------------------------------------------

// Elevator
// -------------------------------------------------------------------------
bool NoPassengerOutOrIn(int lift)
{
    if (!StackEmpty(g_elevator[lift].stack_array[g_elevator[lift].current_floor]))
        return false; // sb. prepare to out
    if (g_elevator[lift].state == GOING_DOWN && g_wait_queue[DOWN][g_elevator[lift].current_floor].num && g_elevator[lift].passenger_num < g_elevator[lift].capacity)
        return false; // sb. prepare to down
    if (g_elevator[lift].state == GOING_UP && g_wait_queue[UP][g_elevator[lift].current_floor].num && g_elevator[lift].passenger_num < g_elevator[lift].capacity)
        return false; // sb. prepare to up
    if (g_elevator[lift].in_out_timer > 0)
        return false; // sb. is in or out right now
    return true;
}

bool MoveTimeUp(int lift)
{
    return g_elevator[lift].move_timer == 0;
}

int HigherUpOrDownNotArriveCall(int floor)
{
    int i;
    for (i = floor + 1; i < g_total_floor; ++i)
        if (g_call_down[i] == 1 || g_call_down[i] == -1 ||
            g_call_up[i] == 1 || g_call_up[i] == -1)
            break;
    if (i == g_total_floor)
        return -1;
    return i;
}

int LowerUpOrDownNotArriveCall(int floor)
{
    int i;
    for (i = floor - 1; i >= 0; --i)
        if (g_call_down[i] == 1 || g_call_down[i] == -1 ||
            g_call_up[i] == 1 || g_call_up[i] == -1)
            break;
    return i;
}

int HigherCallCar(int lift)
{
    int i;
    for (i = g_elevator[lift].current_floor + 1; i < g_total_floor; ++i)
        if (g_elevator[lift].call_car[i])
            break;
    if (i == g_total_floor)
        return -1;
    return i;
}

int LowerCallCar(int lift)
{
    int i;
    for (i = g_elevator[lift].current_floor - 1; i >= 0; --i)
        if (g_elevator[lift].call_car[i])
            break;
    return i;
}

bool StopNextFloor(int lift)
{
    // call this function when move is MOVING in ChangeElevatorMove function
    // g_elevator[lift].current_floor has been changed
    // decide whether to stop and whether to change state,
    // if need to stop, set -2, return true
    // if need to change state, set whether_change_state 1
    // if respond to some floor that call is 1, set -1
    if (g_elevator[lift].state == GOING_UP)
    {
        if (g_elevator[lift].current_floor == g_total_floor - 1)
        {
            g_call_down[g_total_floor - 1] = -2;
            g_elevator[lift].whether_change_state = 1;
            return true; // avoid higher than building, it's necessary, not optional!!!!!!!!!!
        }

        if (g_elevator[lift].call_car[g_elevator[lift].current_floor])
        {
            if (HigherCallCar(lift) == -1 && HigherUpOrDownNotArriveCall(g_elevator[lift].current_floor) == -1 &&
                g_call_up[g_elevator[lift].current_floor] != 1 && g_call_up[g_elevator[lift].current_floor] != -1)
            {
                g_elevator[lift].whether_change_state = 1;
                g_call_down[g_elevator[lift].current_floor] = -2;
            }
            else
                g_call_up[g_elevator[lift].current_floor] = -2;
            return true;
        }

        if (g_elevator[lift].passenger_num < g_elevator[lift].capacity &&
            (g_call_up[g_elevator[lift].current_floor] == 1 || g_call_up[g_elevator[lift].current_floor] == -1))
        {
            g_call_up[g_elevator[lift].current_floor] = -2;
            return true;
        }

        if (HigherCallCar(lift) == -1 && g_call_up[g_elevator[lift].current_floor] != 1 && g_call_up[g_elevator[lift].current_floor] != -1)
        {
            int floor = HigherUpOrDownNotArriveCall(g_elevator[lift].current_floor);
            if (floor == -1)
            {
                if (g_call_down[g_elevator[lift].current_floor] != -2)
                {
                    // stop this floor and change state
                    g_elevator[lift].whether_change_state = 1;
                    g_call_down[g_elevator[lift].current_floor] = -2;
                    return true;
                }
            }
            else
            {
                if (g_call_up[floor] == 1)
                    g_call_up[floor] = -1;
                else if (g_call_down[floor] == 1)
                    g_call_down[floor] = -1;
            }
        }

        return false;
    }
    else
    {
        if (g_elevator[lift].current_floor == 0)
        {
            g_call_up[0] = -2;
            g_elevator[lift].whether_change_state = 1;
            return true; // avoid higher than building, it's necessary, not optional!!!!!!!!!!
        }

        if (g_elevator[lift].call_car[g_elevator[lift].current_floor])
        {
            if (LowerCallCar(lift) == -1 && LowerUpOrDownNotArriveCall(g_elevator[lift].current_floor) == -1 &&
                g_call_down[g_elevator[lift].current_floor] != 1 && g_call_down[g_elevator[lift].current_floor] != -1)
            {
                g_elevator[lift].whether_change_state = 1;
                g_call_up[g_elevator[lift].current_floor] = -2;
            }
            else
                g_call_down[g_elevator[lift].current_floor] = -2;
            return true;
        }

        if (g_elevator[lift].passenger_num < g_elevator[lift].capacity &&
            (g_call_down[g_elevator[lift].current_floor] == 1 || g_call_down[g_elevator[lift].current_floor] == -1))
        {
            g_call_down[g_elevator[lift].current_floor] = -2;
            return true;
        }

        if (LowerCallCar(lift) == -1 && g_call_down[g_elevator[lift].current_floor] != 1 && g_call_down[g_elevator[lift].current_floor] != -1)
        {
            int floor = LowerUpOrDownNotArriveCall(g_elevator[lift].current_floor);
            if (floor == -1)
            {
                if (g_call_up[g_elevator[lift].current_floor] != -2)
                {
                    // stop this floor and change state
                    g_elevator[lift].whether_change_state = 1;
                    g_call_up[g_elevator[lift].current_floor] = -2;
                    return true;
                }
            }
            else
            {
                if (g_call_down[floor] == 1)
                    g_call_down[floor] = -1;
                else if (g_call_up[floor] == 1)
                    g_call_up[floor] = -1;
            }
        }

        return false;
    }
}

void ChangeElevatorMove(int lift)
{
    switch (g_elevator[lift].move)
    {
    case OPENING:
        g_elevator[lift].move = OPENED;
        g_elevator[lift].move_timer = CLOSING_TEST_TIME;
        break;

    case OPENED:
        if (NoPassengerOutOrIn(lift))
        {
            g_elevator[lift].move = CLOSING;
            g_elevator[lift].move_timer = DOOR_TIME;
            Show(DOOR_CLOSING, -1, -1, -1, lift, g_elevator[lift].current_floor);
        }
        else
            g_elevator[lift].move_timer = CLOSING_TEST_TIME;
        break;

    case CLOSING:
        if (g_elevator[lift].passenger_num == g_elevator[lift].capacity)
            g_elevator[lift].move = CLOSED;
        else if ((g_elevator[lift].state == GOING_UP && g_wait_queue[UP][g_elevator[lift].current_floor].num) ||
                 (g_elevator[lift].state == GOING_DOWN && g_wait_queue[DOWN][g_elevator[lift].current_floor].num))
        {
            g_elevator[lift].move = OPENING;
            g_elevator[lift].move_timer = DOOR_TIME;
            Show(DOOR_REOPENING, -1, -1, -1, lift, g_elevator[lift].current_floor);
        }
        else
            g_elevator[lift].move = CLOSED;
        break;

    case CLOSED:
    {
        if (g_elevator[lift].state == GOING_UP)
        {
            // decided to go up when arrived just now
            // deal with button
            if (g_wait_queue[UP][g_elevator[lift].current_floor].num)
                g_call_up[g_elevator[lift].current_floor] = 1;
            else
                g_call_up[g_elevator[lift].current_floor] = 0;

            int i;
            for (i = g_elevator[lift].current_floor + 1; i < g_total_floor; ++i)
            {
                if (g_elevator[lift].call_car[i])
                {
                    g_elevator[lift].move = ACCELERATING;
                    g_elevator[lift].move_timer = ACCELERATE_TIME;
                    Show(ELEVATOR_ACCELERATING, -1, -1, -1, lift, g_elevator[lift].current_floor);
                    break;
                }
            }
            if (i == g_total_floor)
            {
                // though maybe there is someone in queue of higher floor than floor of this lift,
                // we can still let this lift be waiting and wake up it in the next loop in main function
                g_elevator[lift].move = WAITING;
                g_elevator[lift].state = IDLE;
                g_elevator[lift].move_timer = MAX_WAITING_TIME;
                Show(BE_IDLE, -1, -1, -1, lift, g_elevator[lift].current_floor);
            }
        }
        else if (g_elevator[lift].state == GOING_DOWN)
        {
            if (g_wait_queue[DOWN][g_elevator[lift].current_floor].num)
                g_call_down[g_elevator[lift].current_floor] = 1;
            else
                g_call_down[g_elevator[lift].current_floor] = 0;

            int i;
            for (i = g_elevator[lift].current_floor - 1; i >= 0; --i)
            {
                if (g_elevator[lift].call_car[i])
                {
                    g_elevator[lift].move = ACCELERATING;
                    g_elevator[lift].move_timer = ACCELERATE_TIME;
                    Show(ELEVATOR_ACCELERATING, -1, -1, -1, lift, g_elevator[lift].current_floor);
                    break;
                }
            }
            if (i == -1)
            {
                // though maybe there is someone in queue of higher floor than floor of this lift,
                // we can still let this lift be waiting and wake up it in the next loop in main function
                g_elevator[lift].move = WAITING;
                g_elevator[lift].state = IDLE;
                g_elevator[lift].move_timer = MAX_WAITING_TIME;
                Show(BE_IDLE, -1, -1, -1, lift, g_elevator[lift].current_floor);
            }
        }
        else
            exit(UNKNOWN_ERROR);
        break;
    }

    case ACCELERATING:
        g_elevator[lift].move = MOVING;
        if (g_elevator[lift].state == GOING_UP)
            g_elevator[lift].move_timer = UP_TIME;
        else
            g_elevator[lift].move_timer = DOWN_TIME;
        Show(IS_MOVING, -1, -1, -1, lift, g_elevator[lift].current_floor);
        break;

    case MOVING:
        if (g_elevator[lift].state == GOING_UP)
        {
            ++g_elevator[lift].current_floor;
            if (StopNextFloor(lift))
            {
                g_elevator[lift].move = SLOWING_DOWN;
                g_elevator[lift].move_timer = UP_SLOW_TIME;
                Show(IS_SLOWING_DOWN, -1, -1, -1, lift, g_elevator[lift].current_floor);
            }
            else
            {
                g_elevator[lift].move_timer = UP_TIME;
                Show(IS_MOVING, -1, -1, -1, lift, g_elevator[lift].current_floor);
            }
        }
        else if (g_elevator[lift].state == GOING_DOWN)
        {
            --g_elevator[lift].current_floor;
            if (StopNextFloor(lift))
            {
                g_elevator[lift].move = SLOWING_DOWN;
                g_elevator[lift].move_timer = DOWN_SLOW_TIME;
                Show(IS_SLOWING_DOWN, -1, -1, -1, lift, g_elevator[lift].current_floor);
            }
            else
            {
                g_elevator[lift].move_timer = DOWN_TIME;
                Show(IS_MOVING, -1, -1, -1, lift, g_elevator[lift].current_floor);
            }
        }
        else
            exit(UNKNOWN_ERROR);
        break;

    case SLOWING_DOWN:
    {
        // g_call_up or g_call_down has been changed to -2 in StopNextFloor function
        g_elevator[lift].call_car[g_elevator[lift].current_floor] = 0;
        g_elevator[lift].move = OPENING;
        g_elevator[lift].move_timer = DOOR_TIME;
        Show(ARRIVE_AND_OPENING, -1, -1, -1, lift, g_elevator[lift].current_floor);

        // judge whether change state
        if (g_elevator[lift].state == GOING_UP)
        {
            if (g_elevator[lift].whether_change_state)
                g_elevator[lift].state = GOING_DOWN;
            g_elevator[lift].whether_change_state = 0;
        }
        else if (g_elevator[lift].state == GOING_DOWN)
        {
            if (g_elevator[lift].whether_change_state)
                g_elevator[lift].state = GOING_UP;
            g_elevator[lift].whether_change_state = 0;
        }
        else
            exit(UNKNOWN_ERROR);
        break;
    }

    case WAITING:
        if (g_elevator[lift].current_floor == g_elevator[lift].idle_floor)
            g_elevator[lift].move_timer = MAX_WAITING_TIME;
        else
        {
            if (g_elevator[lift].current_floor > g_elevator[lift].idle_floor)
                g_elevator[lift].state = GOING_DOWN;
            else
                g_elevator[lift].state = GOING_UP;
            g_elevator[lift].call_car[g_elevator[lift].idle_floor] = 1;
            g_elevator[lift].move_timer = ACCELERATE_TIME;
            g_elevator[lift].move = ACCELERATING;
            Show(IDLE_RETURN, -1, -1, -1, lift, g_elevator[lift].current_floor);
        }
        break;
    }
}

int NearestCall(int floor)
{
    int i = 1;
    while (floor - i >= 0 || floor + i < g_total_floor)
    {
        if ((floor - i >= 0 && (g_call_down[floor - i] == 1 || g_call_up[floor - i] == 1)) ||
            (floor + i < g_total_floor && (g_call_down[floor + i] == 1 || g_call_up[floor + i] == 1)))
            break;
        ++i;
    }
    if (!(floor - i >= 0 || floor + i < g_total_floor))
        return -1;
    if (floor - i >= 0 && (g_call_down[floor - i] == 1 || g_call_up[floor - i] == 1))
        return floor - i;
    return floor + i;
}

void WakeUp()
{
    for (int i = 0; i < g_elevator_num; ++i)
    {
        if (g_elevator[i].move == WAITING)
        {
            // try to wake up the waiting lift
            if (g_call_up[g_elevator[i].current_floor] == 1 || g_call_up[g_elevator[i].current_floor] == -1)
            {
                // change and show
                g_call_up[g_elevator[i].current_floor] = -2;
                g_elevator[i].state = GOING_UP;
                g_elevator[i].move = OPENING;
                g_elevator[i].move_timer = DOOR_TIME;
                Show(IDLE_OPENING, -1, -1, -1, i, g_elevator[i].current_floor);
            }
            else if (g_call_down[g_elevator[i].current_floor] == 1 || g_call_down[g_elevator[i].current_floor] == -1)
            {
                g_call_down[g_elevator[i].current_floor] = -2;
                g_elevator[i].state = GOING_DOWN;
                g_elevator[i].move = OPENING;
                g_elevator[i].move_timer = DOOR_TIME;
                Show(IDLE_OPENING, -1, -1, -1, i, g_elevator[i].current_floor);
            }
            else
            {
                // whether go to other floor
                int nearest = NearestCall(g_elevator[i].current_floor);
                if (nearest != -1)
                {
                    if (nearest > g_elevator[i].current_floor)
                    {
                        // should go up
                        // state is GOING_UP, so make call_up -1 first,
                        // if call_up has been -1, make call_down -1, else don't change call_down
                        if (g_call_up[nearest] == 1)
                            g_call_up[nearest] = -1;
                        else
                            g_call_down[nearest] = -1;
                        g_elevator[i].state = GOING_UP;
                        g_elevator[i].move = ACCELERATING;
                        g_elevator[i].move_timer = ACCELERATE_TIME;
                        Show(ELEVATOR_ACCELERATING, -1, -1, -1, i, g_elevator[i].current_floor);
                    }
                    else
                    {
                        // should go down
                        if (g_call_down[nearest] == 1)
                            g_call_down[nearest] = -1;
                        else
                            g_call_up[nearest] = -1;
                        g_elevator[i].state = GOING_DOWN;
                        g_elevator[i].move = ACCELERATING;
                        g_elevator[i].move_timer = ACCELERATE_TIME;
                        Show(ELEVATOR_ACCELERATING, -1, -1, -1, i, g_elevator[i].current_floor);
                    }
                }
            }
        }
    }
}
// -------------------------------------------------------------------------

// show
void Show(int type, int id, int in_floor, int out_floor, int lift, int lift_floor)
{
    switch (type)
    {
    case NEW_PASSENGER:
        cout << "Time: " << g_current_time << endl;
        cout << id << " comes in Floor " << in_floor << " ";
        cout << "and wanna go to Floor " << out_floor << endl;
        break;

    case GIVE_UP:
        cout << "Time: " << g_current_time << endl;
        cout << id << " gives up" << endl;
        break;

    case PASSENGER_OUT:
        cout << "Time: " << g_current_time << endl;
        cout << id << " get out of Elevator " << lift << " ";
        cout << "in Floor " << lift_floor << " and leave" << endl;
        break;

    case PASSENGER_IN:
        cout << "Time: " << g_current_time << endl;
        cout << id << " get in Elevator " << lift << " ";
        cout << "in Floor " << lift_floor << endl;
        break;

    case DOOR_CLOSING:
        cout << "Time: " << g_current_time << endl;
        cout << "Elevator " << lift << " is closing door ";
        cout << "and has " << g_elevator[lift].passenger_num << " passenger(s)" << endl;
        break;

    case DOOR_REOPENING:
        cout << "Time: " << g_current_time << endl;
        cout << "Elevator " << lift << " is reopening door" << endl;
        break;

    case ELEVATOR_ACCELERATING:
        cout << "Time: " << g_current_time << endl;
        cout << "Elevator " << lift << " accelerating ";
        if (g_elevator[lift].state == GOING_DOWN)
            cout << "down from Floor " << lift_floor << endl;
        else
            cout << "up from Floor " << lift_floor << endl;
        break;

    case BE_IDLE:
        cout << "Time: " << g_current_time << endl;
        cout << "Elevator " << lift << " is idle in Floor " << lift_floor << endl;
        break;

    case IS_MOVING:
        cout << "Time: " << g_current_time << endl;
        cout << "Elevator " << lift << " is moving from Floor " << lift_floor;
        cout << " to Floor ";
        if (g_elevator[lift].state == GOING_DOWN)
            cout << lift_floor - 1 << endl;
        else
            cout << lift_floor + 1 << endl;
        break;

    case IS_SLOWING_DOWN:
        cout << "Time: " << g_current_time << endl;
        cout << "Elevator " << lift << " is slowing down in Floor " << lift_floor << endl;
        break;

    case ARRIVE_AND_OPENING:
        cout << "Time: " << g_current_time << endl;
        cout << "Elevator " << lift << " arrives in Floor " << lift_floor;
        cout << " and is opening door" << endl;
        break;

    case IDLE_RETURN:
        cout << "Time: " << g_current_time << endl;
        cout << "Elevator " << lift << " is idle for a long time and goes back to Floor ";
        cout << g_elevator[lift].idle_floor << " from Floor " << lift_floor << endl;
        break;

    case IDLE_OPENING:
        cout << "Time: " << g_current_time << endl;
        cout << "Elevator " << lift << " is opening door in Floor " << lift_floor << endl;
        break;
    }
    cout << endl;
}

// UserInput
void UserInput()
{
    while (1)
    {
        cout << "Input max run time(>= 500):" << endl;
        cin >> g_max_time;
        if (g_max_time >= 500)
            break;
    }
    while (1)
    {
        cout << "Input the number of elevator(s) (>= 1):" << endl;
        cin >> g_elevator_num;
        if (g_elevator_num >= 1)
            break;
    }
    while (1)
    {
        cout << "Input elevator capacity(>= 1):" << endl;
        cin >> g_capacity;
        if (g_capacity >= 1)
            break;
    }
    while (1)
    {
        cout << "Input the number of floors(>= 3):" << endl;
        cin >> g_total_floor;
        if (g_total_floor >= 3)
            break;
    }
}

// Initialize
void Initialize()
{
    // Elevator
    g_elevator = (ElevatorPtr)malloc(g_elevator_num * sizeof(Elevator));
    if (!g_elevator)
    {
        cout << "overflow" << endl;
        exit(OVERFLOW);
    }
    for (int i = 0; i < g_elevator_num; ++i)
    {
        g_elevator[i].passenger_num = 0;
        g_elevator[i].capacity = g_capacity;
        g_elevator[i].stack_array = (PassengerStackPtr)malloc(g_total_floor * sizeof(PassengerStack));
        if (!g_elevator[i].stack_array)
        {
            cout << "overflow" << endl;
            exit(OVERFLOW);
        }
        for (int j = 0; j < g_total_floor; ++j)
            InitStack(g_elevator[i].stack_array[j]);
        g_elevator[i].passenger_id = (int *)malloc(g_capacity * sizeof(int));
        if (!g_elevator[i].passenger_id)
        {
            cout << "overflow" << endl;
            exit(OVERFLOW);
        }
        g_elevator[i].call_car = (int *)malloc(g_total_floor * sizeof(int));
        if (!g_elevator[i].call_car)
        {
            cout << "overflow" << endl;
            exit(OVERFLOW);
        }
        for (int j = 0; j < g_total_floor; ++j)
            g_elevator[i].call_car[j] = 0;
        if (g_total_floor <= 10)
            g_elevator[i].current_floor = g_elevator[i].idle_floor = 1;
        else
            g_elevator[i].current_floor = g_elevator[i].idle_floor = g_total_floor / 2;
        g_elevator[i].total_floor = g_total_floor;
        g_elevator[i].move = WAITING;
        g_elevator[i].state = IDLE;
        g_elevator[i].move_timer = MAX_WAITING_TIME;
        g_elevator[i].in_out_timer = 0; // must be 0!!!!! To get the first passenger in at once!!! or it may generate bugs!!!
        g_elevator[i].whether_change_state = 0;
    }

    // call_up, call_down
    g_call_down = (int *)malloc(g_total_floor * sizeof(int));
    g_call_up = (int *)malloc(g_total_floor * sizeof(int));
    if (!g_call_up || !g_call_down)
    {
        cout << "overflow" << endl;
        exit(OVERFLOW);
    }
    for (int i = 0; i < g_total_floor; ++i)
        g_call_down[i] = g_call_up[i] = 0;

    // wait queue
    g_wait_queue = (WaitQueuePtr *)malloc(2 * sizeof(WaitQueuePtr));
    if (!g_wait_queue)
    {
        cout << "overflow" << endl;
        exit(OVERFLOW);
    }
    for (int i = 0; i < 2; ++i)
    {
        g_wait_queue[i] = (WaitQueuePtr)malloc(g_total_floor * sizeof(WaitQueue));
        if (!g_wait_queue[i])
        {
            cout << "overflow" << endl;
            exit(OVERFLOW);
        }
    }
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < g_total_floor; ++j)
            InitQueue(g_wait_queue[i][j]);
}

// PrintReady
void PrintReady()
{
    cout << "----------------------------------------------------------" << endl;
    cout << "----------------------------------------------------------" << endl;
    cout << "--------------------------Ready---------------------------" << endl;
    cout << "----------------------------------------------------------" << endl;
    cout << "----------------------------------------------------------" << endl;
}

void PrintEnd()
{
    cout << "----------------------------------------------------------" << endl;
    cout << "----------------------------------------------------------" << endl;
    cout << "---------------------------End----------------------------" << endl;
    cout << "----------------------------------------------------------" << endl;
    cout << "----------------------------------------------------------" << endl;
    cout << "    new: " << g_new_num << endl;
    cout << "     in: " << g_get_in_num << endl;
    cout << "    out: " << g_get_out_num << endl;
    cout << "give up: " << g_give_up_num << endl;
}

void AllClear()
{
    // call_up, call_down
    free(g_call_down);
    free(g_call_up);

    // wait queue
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < g_total_floor; ++j)
            DestroyQueue(g_wait_queue[i][j]);
    for (int i = 0; i < 2; ++i)
        free(g_wait_queue[i]);
    free(g_wait_queue);

    // Elevator
    for (int i = 0; i < g_elevator_num; ++i)
    {
        free(g_elevator[i].call_car);
        free(g_elevator[i].passenger_id);
        for (int j = 0; j < g_total_floor; ++j)
            DestroyStack(g_elevator[i].stack_array[j]);
        free(g_elevator[i].stack_array);
    }
    free(g_elevator);
}

void Simulate()
{
    UserInput();
    Initialize();
    srand((unsigned)time(NULL)); // just need to srand one time
    PrintReady();
    while (g_current_time < g_max_time)
    {
        if (g_next_passenger_inter_time == 0)
            NewPassenger();
        else
            --g_next_passenger_inter_time;
        CheckGiveUp();
        WakeUp();
        for (int i = 0; i < g_elevator_num; ++i)
        {
            if (g_elevator[i].move == OPENED)
            {
                if (g_elevator[i].in_out_timer == 0)
                {
                    bool out, in;
                    // first out, then in
                    if (!(out = PassengerOut(i)))
                        in = PassengerIn(i);
                    if (out || in)
                        g_elevator[i].in_out_timer = IN_OUT_TIME;
                }
                else
                    --g_elevator[i].in_out_timer;
            }
            if (MoveTimeUp(i))
                ChangeElevatorMove(i);
            else
                --g_elevator[i].move_timer;
        }
        ++g_current_time;
    }
    PrintEnd();
    AllClear();
}

int main()
{
    Simulate();
    system("pause");
    return 0;
}