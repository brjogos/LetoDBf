/*
 * Leto db server functions
 *
 * Copyright 2010 Alexander S. Kresin <alex / at / belacy.belgorod.su>
 *
 * modification/ enhancement/ thread safetiness etc. ..
 *           2016 Rolf 'elch' Beckmann
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307 USA (or visit the web site http://www.gnu.org/).
 *
 * As a special exception, the Harbour Project gives permission for
 * additional uses of the text contained in its release of Harbour.
 *
 * The exception is that, if you link the Harbour libraries with other
 * files to produce an executable, this does not by itself cause the
 * resulting executable to be covered by the GNU General Public License.
 * Your use of that executable is in no way restricted on account of
 * linking the Harbour library code into it.
 *
 * This exception does not however invalidate any other reasons why
 * the executable file might be covered by the GNU General Public License.
 *
 * This exception applies only to the code released by the Harbour
 * Project under the name Harbour.  If you copy code from other
 * Harbour Project or Free Software Foundation releases into a copy of
 * Harbour, as the General Public License permits, the exception does
 * not apply to the code that you add in this way.  To avoid misleading
 * anyone as to the status of such modified files, you must delete
 * this exception notice from them.
 *
 * If you write modifications of your own for Harbour, it is your choice
 * whether to permit this exception to apply to your modifications.
 * If you do not wish that, delete this exception notice.
 *
 */

#include "srvleto.h"

#define VARGROUPS_ALLOC  10
#define VARS_ALLOC       10

#define LETOVAR_LOG      '1'
#define LETOVAR_NUM      '2'
#define LETOVAR_STR      '3'
#define LETOVAR_ARR      '4'

struct leto_struLogical
{
   HB_BOOL value;
};

struct leto_struLong
{
   long value;
};

struct leto_struDouble
{
   double value;
};

struct leto_struString
{
   HB_ULONG length;
   HB_ULONG allocated;
   char *   value;
};

typedef struct
{
   char *    szName;
   HB_USHORT uiNameLen;
   HB_USHORT uiUser;
   char      type;
   char      cFlag;
   char      bDouble;
   union
   {
      struct leto_struLogical asLogical;
      struct leto_struLong    asLong;
      struct leto_struDouble  asDouble;
      struct leto_struString  asString;
   } item;
} LETO_VAR;

typedef struct
{
   char *     szName;
   HB_USHORT  uiNameLen;
   HB_USHORT  uiItems;
   HB_USHORT  uiAlloc;
   LETO_VAR * pItems;
} LETO_VARGROUPS;

static const char * szOk = "++++";
static const char * szErr1 = "-001";
static const char * szErr2 = "-002";
static const char * szErr3 = "-003";
static const char * szErr4 = "-004";
static const char * szErrAcc = "-ACC";

static LETO_VARGROUPS * s_pVarGroups = NULL;
static HB_USHORT        s_uiVarGroupsAlloc = 0;
static HB_USHORT        s_uiVarGroupsCurr = 0;
static HB_ULONG         s_ulVarsCurr = 0;
static HB_ULONG         s_ulVarsMax = 1000;
static HB_USHORT        s_uiVarsOwnMax = 50;
static HB_ULONG         s_ulVarLenAll = 0;
static HB_ULONG         s_ulVarLenAllMax = 0x4000000;  /* 64 MB size of all strings/ arrays in sum */

#if defined( HB_SPINLOCK_INIT ) && ! defined( HB_HELGRIND_FRIENDLY )
   static HB_SPINLOCK_T s_VarMtx = HB_SPINLOCK_INIT;
   #define HB_GC_LOCKV()    HB_SPINLOCK_ACQUIRE( &s_VarMtx )
   #define HB_GC_UNLOCKV()  HB_SPINLOCK_RELEASE( &s_VarMtx )
#else
   static HB_CRITICAL_NEW( s_VarMtx );
   #define HB_GC_LOCKV()       hb_threadEnterCriticalSection( &s_VarMtx )
   #define HB_GC_UNLOCKV()     hb_threadLeaveCriticalSection( &s_VarMtx )
#endif

extern int leto_GetParam( const char * szData, const char ** pp2, const char ** pp3, const char ** pp4, const char ** pp5 );
extern void leto_SendAnswer( PUSERSTRU pUStru, const char * szData, HB_ULONG ulLen );
extern PUSERSTRU letoGetsUStru( void );  /* fetch the static TLS in letofunc */
extern void leto_wUsLog( PUSERSTRU pUStru, int n, const char* s, ... );

static PHB_ITEM leto_var_ret( LETO_VAR * pItem );


/* for TSD thread local storage last used pItem cache, e.g. for use in filter conditions */
static void leto_InitVarCache( void * cargo )
{
   PHB_ITEM * ppItemTSD = ( PHB_ITEM * ) cargo;

   *ppItemTSD = NULL;
}

static void leto_DeinitVarCache( void * cargo )
{
   PHB_ITEM * ppItemTSD = ( PHB_ITEM * ) cargo;

   if( *ppItemTSD )
   {
      hb_itemRelease( *ppItemTSD );
      *ppItemTSD = NULL;
   }
}

static HB_TSD_NEW( s_TSDitem, sizeof( PHB_ITEM * ), leto_InitVarCache, leto_DeinitVarCache );

static _HB_INLINE_ PHB_ITEM leto_GetVarCached( void )
{
   return *( PHB_ITEM * ) hb_stackGetTSD( &s_TSDitem );
}

static void leto_SetVarCache( LETO_VAR * pVarItem )
{
   PHB_ITEM * ppItemTSD = hb_stackGetTSD( &s_TSDitem );

   if( *ppItemTSD )
      hb_itemRelease( *ppItemTSD );
   *ppItemTSD = leto_var_ret( pVarItem );
}

static void leto_ClearVarCache( void )
{
   PHB_ITEM * ppItemTSD = hb_stackGetTSD( &s_TSDitem );

   if( ppItemTSD )
   {
      hb_itemRelease( *ppItemTSD );
      *ppItemTSD = NULL;
   }
}

/* retrieves THREAD LOCAL last leto_Var[Set|Get|Inc|Dec] *value* */
HB_FUNC( LETO_VARGETCACHED )
{
   hb_itemReturn( leto_GetVarCached() );
}


/* during server start, no mutex need */
void leto_setVarsMax( HB_ULONG ulMaxVars, HB_ULONG ulMaxVarLen )
{
   s_ulVarsMax = ulMaxVars;
   s_ulVarLenAllMax = ulMaxVarLen;
}

static LETO_VAR * leto_var_create( PUSERSTRU pUStru, LETO_VARGROUPS * pGroup, const char * pp1, const char * pp2, char cFlag )
{
   LETO_VAR * pItem;
   HB_USHORT  uiVarGroupLen, uiVarLen;

   if( ( cFlag & LETO_VOWN ) && ( s_uiVarsOwnMax <= pUStru->uiVarsOwnCurr ) )
      return NULL;

   if( ! pGroup )
   {
      if( ! s_pVarGroups )
      {
         s_uiVarGroupsAlloc = VARGROUPS_ALLOC;
         s_pVarGroups = ( LETO_VARGROUPS * ) hb_xgrabz( sizeof( LETO_VARGROUPS ) * s_uiVarGroupsAlloc );
      }
      else if( s_uiVarGroupsAlloc == s_uiVarGroupsCurr )
      {
         s_pVarGroups = ( LETO_VARGROUPS * ) hb_xrealloc( s_pVarGroups, sizeof( LETO_VARGROUPS ) * ( s_uiVarGroupsAlloc + VARGROUPS_ALLOC ) );
         memset( s_pVarGroups + s_uiVarGroupsAlloc, 0, sizeof( LETO_VARGROUPS ) * VARGROUPS_ALLOC );
         s_uiVarGroupsAlloc += VARGROUPS_ALLOC;
      }
      pGroup = s_pVarGroups;
      while( ( pGroup - s_pVarGroups ) < s_uiVarGroupsAlloc && pGroup->szName )
         pGroup++;

      if( ( pGroup - s_pVarGroups ) < s_uiVarGroupsAlloc )
      {
         s_uiVarGroupsCurr++;

         uiVarGroupLen = ( HB_USHORT ) strlen( pp1 );
         pGroup->szName = ( char * ) hb_xgrab( uiVarGroupLen + 1 );
         memcpy( pGroup->szName, pp1, uiVarGroupLen );
         pGroup->szName[ uiVarGroupLen ] = '\0';
         pGroup->uiNameLen = uiVarGroupLen;
         pGroup->uiAlloc = VARS_ALLOC;
         pGroup->pItems = ( LETO_VAR * ) hb_xgrabz( sizeof( LETO_VAR ) * pGroup->uiAlloc );
         s_ulVarsCurr += VARS_ALLOC;
      }
      else
         return NULL;
   }
   if( pGroup->uiAlloc == pGroup->uiItems )
   {
      pGroup->pItems = ( LETO_VAR * ) hb_xrealloc( pGroup->pItems, sizeof( LETO_VAR ) * ( pGroup->uiAlloc + VARS_ALLOC ) );
      memset( pGroup->pItems + pGroup->uiAlloc, 0, sizeof( LETO_VAR ) * VARS_ALLOC );
      pGroup->uiAlloc += VARS_ALLOC;
      s_ulVarsCurr += VARS_ALLOC;
   }
   pItem = pGroup->pItems;
   while( ( pItem - pGroup->pItems ) < pGroup->uiAlloc && pItem->szName )
      pItem++;

   if( ( pItem - pGroup->pItems ) < pGroup->uiAlloc )
   {
      pGroup->uiItems++;

      memset( pItem, 0, sizeof( LETO_VAR ) );
      uiVarLen = ( HB_USHORT ) strlen( pp2 );
      pItem->szName = ( char * ) hb_xgrab( uiVarLen + 1 );
      memcpy( pItem->szName, pp2, uiVarLen );
      pItem->szName[ uiVarLen ] = '\0';
      pItem->uiNameLen = uiVarLen;
      pItem->cFlag = cFlag;
      if( pItem->cFlag & LETO_VOWN )
      {
         pItem->uiUser = ( HB_USHORT ) pUStru->iUserStru;
         if( ! pUStru->pVarLink )
            pUStru->pVarLink = ( VAR_LINK * ) hb_xgrabz( sizeof( VAR_LINK ) * s_uiVarsOwnMax );
         ( pUStru->pVarLink + pUStru->uiVarsOwnCurr )->uiGroup = ( HB_USHORT ) ( ( pGroup - s_pVarGroups ) + 1 );
         ( pUStru->pVarLink + pUStru->uiVarsOwnCurr )->uiVar = ( HB_USHORT ) ( ( pItem - pGroup->pItems ) + 1 );
         pUStru->uiVarsOwnCurr++;
      }
      return pItem;
   }
   else
      return NULL;
}

static char * leto_var_get( LETO_VAR * pItem, HB_ULONG * ulLen, HB_BOOL bWithPrefix )
{
   char * pData = NULL;

   if( pItem )
   {
      char * ptr = pData = ( char * ) hb_xgrab( 32 );
      int    iPrefix;
      long   lVarLen = 0;

      if( bWithPrefix )
      {
         *ptr++ = '+';
         *ptr++ = pItem->type;
         *ptr++ = ';';
         iPrefix = 3;
      }
      else
         iPrefix = 0;

      switch( pItem->type )
      {
         case LETOVAR_LOG:
            *ptr = ( pItem->item.asLogical.value ) ? '1' : '0';
            lVarLen = 1;
            break;

         case LETOVAR_NUM:
            if( pItem->bDouble == '0' )
               lVarLen = sprintf( ptr, "%ld", pItem->item.asLong.value );
            else
               lVarLen = sprintf( ptr, "%f", pItem->item.asDouble.value );
            break;

         case LETOVAR_STR:
         case LETOVAR_ARR:
            lVarLen = pItem->item.asString.length;
            if( *ulLen > 0 && *ulLen < ( HB_ULONG ) lVarLen )
               lVarLen = ( long ) *ulLen;
            if( lVarLen > 32 - iPrefix )
               pData = ( char * ) hb_xrealloc( pData, lVarLen + iPrefix );
            memcpy( pData + iPrefix, pItem->item.asString.value, lVarLen );
            break;
      }

      *ulLen = ( HB_ULONG ) ( iPrefix + lVarLen );
   }
   else
      *ulLen = 0;

   return pData;
}

static void leto_var_del( PUSERSTRU pUStru, LETO_VARGROUPS * pGroup, HB_USHORT uiItem )
{
   LETO_VAR * pItem = pGroup->pItems + uiItem;

   if( pItem->szName )
   {
      leto_ClearVarCache();
      hb_xfree( pItem->szName );
      if( ( pItem->type == LETOVAR_STR || pItem->type == LETOVAR_ARR ) && pItem->item.asString.value )
      {
         s_ulVarLenAll -= pItem->item.asString.length;
         hb_xfree( pItem->item.asString.value );
      }

      if( pUStru && pUStru->pVarLink && ( pItem->cFlag & LETO_VOWN ) )
      {
         HB_USHORT  uiGroup = ( HB_USHORT ) ( ( pGroup - s_pVarGroups ) + 1 );
         HB_USHORT  uiItem2 = ( HB_USHORT ) ( ( pItem - pGroup->pItems ) + 1 ), ui;
         VAR_LINK * pVLink = pUStru->pVarLink;

         for( ui = 0; ui < pUStru->uiVarsOwnCurr; ui++, pVLink++ )
         {
            if( pVLink->uiGroup == uiGroup && pVLink->uiVar == uiItem2 )
            {
               memmove( pVLink, pVLink + 1, sizeof( VAR_LINK ) * ( pUStru->uiVarsOwnCurr - ui - 1 ) );
               pUStru->uiVarsOwnCurr--;
               break;
            }
         }
      }

      memset( pItem, 0, sizeof( LETO_VAR ) );
      pGroup->uiItems--;
      if( ! pGroup->uiItems )
      {
         s_uiVarGroupsCurr--;
         s_ulVarsCurr -= pGroup->uiAlloc;
         hb_xfree( pGroup->szName );
         hb_xfree( pGroup->pItems );
         memset( pGroup, 0, sizeof( LETO_VARGROUPS ) );
      }
   }
}

static void leto_var_delgroup( PUSERSTRU pUStru, LETO_VARGROUPS * pGroup )
{
   HB_USHORT uiItem = 0;

   while( uiItem < pGroup->uiAlloc )
   {
      leto_var_del( pUStru, pGroup, uiItem++ );
      if( ! pGroup->uiItems )
         break;
   }
}

/* need HB_GC_LOCKV() */
static LETO_VAR * leto_var_find( const char * pVarGroup, const char * pVar, LETO_VARGROUPS ** ppGroup, HB_USHORT * puiItem )
{
   LETO_VARGROUPS * pGroup;
   LETO_VAR *       pItem = NULL;
   HB_USHORT        uiGroups = 0, uiGroup, uiItem, ui;

   for( uiGroup = 0; uiGroup < s_uiVarGroupsAlloc; uiGroup++ )
   {
      pGroup = s_pVarGroups + uiGroup;
      if( pGroup->uiNameLen )
      {
         if( ! leto_stricmp( pVarGroup, pGroup->szName ) )
         {
            *ppGroup = pGroup;
            if( *pVar )
            {
               for( uiItem = 0, ui = 0; uiItem < pGroup->uiAlloc; uiItem++ )
               {
                  if( ( pGroup->pItems + uiItem )->szName )
                  {
                     if( ! leto_stricmp( pVar, ( pGroup->pItems + uiItem )->szName ) )
                     {
                        pItem = pGroup->pItems + uiItem;
                        *puiItem = uiItem;
                        break;
                     }
                     if( ++ui >= pGroup->uiItems )
                        break;
                  }
               }
            }
            break;
         }
         if( ++uiGroups >= s_uiVarGroupsCurr )
            break;
      }
   }

   return pItem;
}

static void leto_var_set_str( LETO_VAR * pItem, const char * pStr, HB_ULONG ulLen )
{
   if( pItem->item.asString.allocated && pItem->item.asString.allocated < ulLen )
   {
      s_ulVarLenAll -= pItem->item.asString.length;
      hb_xfree( pItem->item.asString.value );
      pItem->item.asString.allocated = 0;
   }
   if( ! pItem->item.asString.allocated )
   {
      pItem->item.asString.value = ( char * ) hb_xgrab( ulLen + 1 );
      pItem->item.asString.allocated = ulLen + 1;
   }
   pItem->item.asString.length = ulLen;
   memcpy( pItem->item.asString.value, pStr, ulLen );
   pItem->item.asString.value[ ulLen ] = '\0';
   s_ulVarLenAll += ulLen;
}

static _HB_INLINE_ HB_BOOL leto_var_accessdeny( PUSERSTRU pUStru, LETO_VAR * pItem, HB_UCHAR cFlag )
{
   return ( pItem->cFlag & LETO_VOWN ) &&
          ( pItem->cFlag & cFlag ) &&
          ( pItem->uiUser != pUStru->iUserStru );
}

void leto_Variables( PUSERSTRU pUStru, const char * szData )
{
   char *       pData = NULL;
   const char * pVarGroup, * pVar, * pp3, * pp4;
   int          nParam = leto_GetParam( szData, &pVarGroup, &pVar, &pp3, NULL );
   HB_USHORT    uiGroup, uiItem = 0, uiLen, ui;
   HB_ULONG     ulLen;

   if( nParam < 3 )
      leto_SendAnswer( pUStru, szErr2, 4 );
   else
   {
      LETO_VARGROUPS * pGroup = NULL;
      LETO_VAR *       pItem;
      char             cFlag1, cFlag2;

      HB_GC_LOCKV();

      if( *pVarGroup )
         pItem = leto_var_find( pVarGroup, pVar, &pGroup, &uiItem );
      else
         pItem = NULL;

      if( *szData == LETOSUB_set )
      {
         HB_UCHAR uLenLen;
         HB_ULONG pValLength = 0;

         cFlag1 = *( pp3 + 1 );
         cFlag2 = *( pp3 + 2 );
         pp4 = pp3 + 4;

         if( nParam == 4 && ( uLenLen = ( ( ( HB_UCHAR ) *pp4 ) & 0xFF ) ) < 10 )
         {
            nParam++;
            pValLength = leto_b2n( pp4 + 1, uLenLen );
            pp4 += uLenLen + 1;
         }

         if( nParam < 5 || ! pValLength || ! *pVarGroup || ! *pVar || *pp3 < '1' || cFlag1 < ' ' || cFlag2 < ' ' )
            leto_SendAnswer( pUStru, szErr2, 4 );
         else if( ! pItem && ( ! ( cFlag1 & LETO_VCREAT ) || s_ulVarsCurr >= s_ulVarsMax ) )
            leto_SendAnswer( pUStru, szErr3, 4 );
         else
         {
            if( ! pItem )
               pItem = leto_var_create( pUStru, pGroup, pVarGroup, pVar, cFlag1 );
            if( ! pItem )
               leto_SendAnswer( pUStru, szErr3, 4 );
            else if( pItem->type >= '1' && pItem->type != *pp3 )
               leto_SendAnswer( pUStru, szErr4, 4 );
            else if( leto_var_accessdeny( pUStru, pItem, LETO_VDENYWR | LETO_VCREAT ) )
               leto_SendAnswer( pUStru, szErrAcc, 4 );
            else
            {
               if( ( *pp3 == LETOVAR_STR || *pp3 == LETOVAR_ARR ) && ( pValLength > s_ulVarLenAllMax >> 2 ||
                   s_ulVarLenAll + pValLength - pItem->item.asString.length > s_ulVarLenAllMax ) )
                  leto_SendAnswer( pUStru, szErr3, 4 );
               else if( *pp3 > LETOVAR_ARR )
                  leto_SendAnswer( pUStru, szErr2, 4 );
               else
               {
                  HB_ULONG ulRetLen = 0;

                  if( cFlag2 & LETO_VPREVIOUS )
                     pData = leto_var_get( pItem, &ulRetLen, HB_TRUE );

                  if( *pp3 == LETOVAR_LOG )
                     pItem->item.asLogical.value = ( *pp4 != '0' );
                  else if( *pp3 == LETOVAR_NUM )
                  {
                     const char * ptr2 = pp4;
                     HB_BOOL      bDec = HB_FALSE;

                     while( *ptr2 )
                     {
                        if( *ptr2++ == '.' )
                        {
                           bDec = HB_TRUE;
                           break;
                        }
                     }
                     if( ! bDec )
                     {
                        pItem->item.asLong.value = atol( pp4 );
                        pItem->bDouble = '0';
                     }
                     else
                     {
                        pItem->item.asDouble.value = atof( pp4 );
                        pItem->bDouble = '1';
                     }
                  }
                  else if( *pp3 == LETOVAR_STR || *pp3 == LETOVAR_ARR )
                     leto_var_set_str( pItem, pp4, pValLength );

                  pItem->type = *pp3;
                  if( cFlag2 & LETO_VPREVIOUS )
                  {
                     if( pData )
                        leto_SendAnswer( pUStru, pData, ulRetLen );
                     else
                        leto_SendAnswer( pUStru, "+?;", 3 );
                  }
                  else
                     leto_SendAnswer( pUStru, szOk, 4 );
                  leto_SetVarCache( pItem );
               }
            }
         }
      }
      else if( *szData == LETOSUB_get )
      {
         ulLen = 0;
         if( ! pItem )
            leto_SendAnswer( pUStru, szErr3, 4 );
         else if( leto_var_accessdeny( pUStru, pItem, LETO_VDENYRD ) )
            leto_SendAnswer( pUStru, szErrAcc, 4 );
         else if( ( pData = leto_var_get( pItem, &ulLen, HB_TRUE ) ) == NULL )
            leto_SendAnswer( pUStru, szErr4, 4 );
         else
            leto_SendAnswer( pUStru, pData, ulLen );
      }
      else if( *szData == LETOSUB_inc || *szData == LETOSUB_dec )
      {
         HB_BOOL bInc = ( *szData == LETOSUB_inc );

         ulLen = 0;
         cFlag1 = *( pp3 + 1 );
         cFlag2 = *( pp3 + 2 );
         if( nParam < 4 || ! *pVarGroup || ! *pVar || *pp3 != '2' || cFlag1 < ' ' || cFlag2 < ' ' )
            leto_SendAnswer( pUStru, szErr2, 4 );
         else if( ! pItem && ( ! ( cFlag1 & LETO_VCREAT ) || s_ulVarsCurr >= s_ulVarsMax ) )
            leto_SendAnswer( pUStru, szErr3, 4 );
         else
         {
            if( ! pItem )
            {
               pItem = leto_var_create( pUStru, pGroup, pVarGroup, pVar, cFlag1 );
               if( pItem )
               {
                  pItem->type = LETOVAR_NUM;
                  pItem->bDouble = '0';
                  pItem->item.asLong.value = 0;
               }
            }
            if( ! pItem )
               leto_SendAnswer( pUStru, szErr3, 4 );
            else if( pItem->type != LETOVAR_NUM )
               leto_SendAnswer( pUStru, szErr4, 4 );
            else if( pItem->bDouble != '0' )
               leto_SendAnswer( pUStru, szErr2, 4 );
            else if( leto_var_accessdeny( pUStru, pItem, LETO_VDENYWR ) )
               leto_SendAnswer( pUStru, szErrAcc, 4 );
            else
            {
               if( cFlag2 & LETO_VPREVIOUS )
               {
                  if( ( pData = leto_var_get( pItem, &ulLen, HB_TRUE ) ) != NULL )
                     leto_SendAnswer( pUStru, pData, ulLen );
                  else
                     leto_SendAnswer( pUStru, szErr1, 4 );
               }

               if( bInc )
                  pItem->item.asLong.value++;
               else
                  pItem->item.asLong.value--;

               if( ! ( cFlag2 & LETO_VPREVIOUS ) )
               {
                  if( ( pData = leto_var_get( pItem, &ulLen, HB_TRUE ) ) != NULL )
                     leto_SendAnswer( pUStru, pData, ulLen );
                  else
                     leto_SendAnswer( pUStru, szErr1, 4 );
               }

               leto_SetVarCache( pItem );
            }
         }
      }
      else if( *szData == LETOSUB_del )
      {
         if( ! pItem )
         {
            if( *pVar )
               leto_SendAnswer( pUStru, szErr3, 4 );
            else
            {
               if( pGroup )
               {
                  leto_var_delgroup( pUStru, pGroup );
                  leto_SendAnswer( pUStru, szOk, 4 );
               }
               else
                  leto_SendAnswer( pUStru, szErr3, 4 );
            }
         }
         else
         {
            leto_var_del( pUStru, pGroup, uiItem );
            leto_SendAnswer( pUStru, szOk, 4 );
         }
      }
      else if( *szData == LETOSUB_list )
      {
         char *    ptrTmp;
         HB_USHORT uiItemCount = 0;

         ulLen = 0;
         if( *pVarGroup )
         {
            if( pGroup )  /* vars in group */
            {
               HB_ULONG     ulVarLen;
               unsigned int uiMaxLen = 0;
               char *       pVarData;
               HB_UCHAR     uiLenLen;

               if( *pp3 && strlen( pp3 ) < 4 )
                  uiMaxLen = ( unsigned int ) atol( pp3 );
               for( uiItem = 0, ui = 0; uiItem < pGroup->uiAlloc; uiItem++ )
               {
                  pItem = pGroup->pItems + uiItem;
                  if( pItem->szName )
                  {
                     if( ! ( uiMaxLen && leto_var_accessdeny( pUStru, pItem, LETO_VDENYRD ) ) )
                     {
                        ulLen += strlen( pItem->szName ) + 1 + 4 + uiMaxLen;
                        uiItemCount++;
                     }
                     if( ++ui >= pGroup->uiItems )
                        break;
                  }
               }
               ptrTmp = pData = ( char * ) hb_xgrab( ulLen + 16 );
               ptrTmp += sprintf( pData, "+%d;", uiItemCount );
               for( uiItem = 0, ui = 0; uiItem < pGroup->uiAlloc; uiItem++ )
               {
                  pItem = pGroup->pItems + uiItem;
                  if( pItem->szName )
                  {
                     uiLen = ( HB_USHORT ) strlen( pItem->szName );
                     memcpy( ptrTmp, pItem->szName, uiLen );
                     ptrTmp += uiLen;
                     if( uiMaxLen )
                     {
                        if( ! leto_var_accessdeny( pUStru, pItem, LETO_VDENYRD ) )
                        {
                           *ptrTmp++ = ';';
                           *ptrTmp++ = pItem->type;
                           *ptrTmp++ = ';';

                           if( pItem->type != LETOVAR_ARR )
                              ulVarLen = uiMaxLen;
                           else
                              ulVarLen = HB_MIN( 5, uiMaxLen );  /* will be symbolic presented */
                           pVarData = leto_var_get( pItem, &ulVarLen, HB_FALSE );
                           /* add length of content */
                           uiLenLen = leto_n2b( ptrTmp + 1, ulVarLen );
                           *ptrTmp = ( uiLenLen & 0xFF );
                           ptrTmp += uiLenLen + 1;

                           memcpy( ptrTmp, pVarData, ulVarLen );
                           if( pVarData )
                              hb_xfree( pVarData );
                           ptrTmp += ulVarLen;
                        }
                     }
                     else
                        *ptrTmp++ = ';';
                     if( ++ui >= pGroup->uiItems )
                        break;
                  }
               }
               *ptrTmp = '\0';
               leto_SendAnswer( pUStru, pData, ptrTmp - pData );
            }
            else
               leto_SendAnswer( pUStru, szErr2, 4 );
         }
         else  /* all vars (all group) */
         {
            for( uiGroup = 0; uiGroup < s_uiVarGroupsAlloc; uiGroup++ )
            {
               if( ( s_pVarGroups + uiGroup )->szName )
               {
                  ulLen += strlen( ( s_pVarGroups + uiGroup )->szName ) + 1;
                  uiItemCount++;
               }
            }
            pData = ( char * ) hb_xgrab( ulLen + 16 );
            ulLen = sprintf( pData, "+%d;", uiItemCount );
            ptrTmp = pData + ulLen;
            for( uiGroup = 0; uiGroup < s_uiVarGroupsCurr; uiGroup++ )
            {
               if( ( s_pVarGroups + uiGroup )->szName )
               {
                  uiLen = ( HB_USHORT ) strlen( ( s_pVarGroups + uiGroup )->szName );
                  memcpy( ptrTmp, ( s_pVarGroups + uiGroup )->szName, uiLen );
                  ptrTmp += uiLen;
                  *ptrTmp++ = ';';
                  ulLen += uiLen + 1;
               }
            }
            leto_SendAnswer( pUStru, pData, ulLen );
         }
      }
      else
         leto_SendAnswer( pUStru, szErr2, 4 );

      HB_GC_UNLOCKV();
   }

   if( pData )
      hb_xfree( pData );
}

void leto_varsown_release( PUSERSTRU pUStru )
{
   if( pUStru->pVarLink )
   {
      if( pUStru->uiVarsOwnCurr )
      {
         VAR_LINK * pVLink;
         HB_USHORT  ui;

         HB_GC_LOCKV();

         if( s_pVarGroups )
         {
            pVLink = pUStru->pVarLink;
            for( ui = 0; ui < pUStru->uiVarsOwnCurr; ui++, pVLink++ )
            {
               if( pVLink->uiGroup && pVLink->uiVar )
                  leto_var_del( NULL, s_pVarGroups + pVLink->uiGroup - 1, pVLink->uiVar - 1 );
            }
         }

         HB_GC_UNLOCKV();
      }
      hb_xfree( pUStru->pVarLink );
      pUStru->pVarLink = NULL;
   }
}

void leto_vars_release( void )
{
   if( s_pVarGroups )
   {
      LETO_VARGROUPS * pGroup = s_pVarGroups;
      HB_USHORT        ui = 0, uiGroups = 0;

      HB_GC_LOCKV();

      while( ui < s_uiVarGroupsAlloc )
      {
         if( pGroup->szName )
         {
            leto_var_delgroup( NULL, pGroup );
            if( ++uiGroups >= s_uiVarGroupsCurr )
               break;
         }
         ui++;
         pGroup++;
      }

      hb_xfree( s_pVarGroups );
      s_pVarGroups = NULL;

      HB_GC_UNLOCKV();
   }
}

static PHB_ITEM leto_var_ret( LETO_VAR * pItem )
{
   PHB_ITEM pReturn = NULL;

   switch( pItem->type )
   {
      case LETOVAR_LOG:
         pReturn = hb_itemNew( NULL );
         hb_itemPutL( pReturn, pItem->item.asLogical.value );
         break;
      case LETOVAR_NUM:
         pReturn = hb_itemNew( NULL );
         if( pItem->bDouble == '0' )
            hb_itemPutNL( pReturn, pItem->item.asLong.value );
         else
            hb_itemPutND( pReturn, pItem->item.asDouble.value );
         break;
      case LETOVAR_STR:
         pReturn = hb_itemNew( NULL );
         hb_itemPutCL( pReturn, pItem->item.asString.value, pItem->item.asString.length );
         break;
      case LETOVAR_ARR:
         if( ! pItem->item.asString.length )
            pReturn = hb_itemArrayNew( 0 );
         else
         {
            HB_SIZE      nSize = pItem->item.asString.length;
            const char * pTmp = pItem->item.asString.value;

            pReturn = hb_itemDeserialize( &pTmp, &nSize );
         }
         break;
   }
   return pReturn;
}

/* leto_udf() */
HB_FUNC( LETO_VARGET )
{
   PUSERSTRU    pUStru = letoGetsUStru();
   const char * pVarGroup = hb_parclen( 1 ) ? hb_parc( 1 ) : NULL;
   const char * pVar = hb_parclen( 2 ) ? hb_parc( 2 ) : NULL;
   LETO_VAR *   pItem;

   HB_GC_LOCKV();

   if( pVarGroup && pVar )
   {
      LETO_VARGROUPS * pGroup = NULL;
      HB_USHORT        uiItem = 0;

      pItem = leto_var_find( pVarGroup, pVar, &pGroup, &uiItem );
      HB_SYMBOL_UNUSED( pGroup );
   }
   else
      pItem = NULL;

   if( pItem && ! leto_var_accessdeny( pUStru, pItem, LETO_VDENYRD ) )
      hb_itemReturnRelease( leto_var_ret( pItem ) );
   else
      hb_ret();

   HB_GC_UNLOCKV();
}

/* leto_udf() */
HB_FUNC( LETO_VARSET )
{
   PUSERSTRU    pUStru = letoGetsUStru();
   const char * pVarGroup = hb_parclen( 1 ) ? hb_parc( 1 ) : NULL;
   const char * pVar = hb_parclen( 2 ) ? hb_parc( 2 ) : NULL;
   LETO_VAR *   pItem = NULL;

   if( pVarGroup && pVar )
   {
      LETO_VARGROUPS * pGroup = NULL;
      HB_USHORT        uiItem = 0;
      char             cFlag1 = HB_ISNUM( 4 ) ? ( char ) hb_parni( 4 ) : 0;

      HB_GC_LOCKV();

      pItem = leto_var_find( pVarGroup, pVar, &pGroup, &uiItem );
      if( ! pItem )
         pItem = leto_var_create( pUStru, pGroup, pVarGroup, pVar, cFlag1 );
      if( pItem && ! leto_var_accessdeny( pUStru, pItem, LETO_VDENYWR + LETO_VCREAT ) )
      {
         if( HB_ISLOG( 3 ) )
         {
            pItem->item.asLogical.value = hb_parl( 3 );
            pItem->type = LETOVAR_LOG;
         }
         else if( HB_ISNUM( 3 ) )
         {
            if( HB_IS_INTEGER( hb_param( 3, HB_IT_ANY ) ) )
            {
               pItem->item.asLong.value = hb_parnl( 3 );
               pItem->bDouble = '0';
            }
            else
            {
               pItem->item.asDouble.value = hb_parnd( 3 );
               pItem->bDouble = '1';
            }
            pItem->type = LETOVAR_NUM;
         }
         else if( HB_ISCHAR( 3 ) )
         {
           if( hb_parclen( 3 ) <= s_ulVarLenAllMax >> 2 &&
               s_ulVarLenAll + hb_parclen( 3 ) - pItem->item.asString.length <= s_ulVarLenAllMax )
              leto_var_set_str( pItem, hb_parc( 3 ), hb_parclen( 3 ) );
           pItem->type = LETOVAR_STR;
         }
         else if( HB_ISARRAY( 3 ) )
         {
            HB_SIZE nSize = 0;
            char *  pArr = hb_itemSerialize( hb_param( 3, HB_IT_ARRAY ), HB_SERIALIZE_NUMSIZE, &nSize );  //  | HB_SERIALIZE_COMPRESS

            if( nSize <= s_ulVarLenAllMax >> 2 &&
                s_ulVarLenAll + nSize - pItem->item.asString.length <= s_ulVarLenAllMax )
               leto_var_set_str( pItem, pArr, nSize );
            pItem->type = LETOVAR_ARR;
            if( pArr )
               hb_xfree( pArr );
         }
      }
      if( pItem )
      {
         leto_SetVarCache( pItem );
         hb_itemReturnRelease( leto_var_ret( pItem ) );
      }

      HB_GC_UNLOCKV();
   }

   if( ! pItem )
      hb_ret();
}

static void leto_var_incdec( HB_BOOL bInc )
{
   PUSERSTRU    pUStru = letoGetsUStru();
   const char * pVarGroup = hb_parclen( 1 ) ? hb_parc( 1 ) : NULL;
   const char * pVar = hb_parclen( 2 ) ? hb_parc( 2 ) : NULL;
   LETO_VAR *   pItem = NULL;

   if( pVarGroup && pVar )
   {
      LETO_VARGROUPS * pGroup = NULL;
      HB_USHORT        uiItem = 0;
      char             cFlag1 = HB_ISNUM( 3 ) ? ( char ) hb_parni( 3 ) : 0;

      HB_GC_LOCKV();

      pItem = leto_var_find( pVarGroup, pVar, &pGroup, &uiItem );
      if( ! pItem && ( cFlag1 & LETO_VCREAT ) )
      {
         pItem = leto_var_create( pUStru, pGroup, pVarGroup, pVar, cFlag1 );
         pItem->bDouble = '0';
      }
      if( pItem && pItem->type == LETOVAR_NUM && pItem->bDouble == '0' &&
          ! leto_var_accessdeny( pUStru, pItem, LETO_VDENYWR ) )
      {
         if( bInc )
            pItem->item.asLong.value++;
         else
            pItem->item.asLong.value--;
      }

      if( pItem )
      {
         leto_SetVarCache( pItem );
         hb_itemReturnRelease( leto_var_ret( pItem ) );
      }

      HB_GC_UNLOCKV();
   }

   if( ! pItem )
      hb_ret();
}

/* leto_udf() */
HB_FUNC( LETO_VARINCR )
{
   leto_var_incdec( HB_TRUE );
}

/* leto_udf() */
HB_FUNC( LETO_VARDECR )
{
   leto_var_incdec( HB_FALSE );
}

/* leto_udf() */
HB_FUNC( LETO_VARDEL )
{
   PUSERSTRU    pUStru = letoGetsUStru();
   const char * pVarGroup = hb_parclen( 1 ) ? hb_parc( 1 ) : NULL;
   const char * pVar = hb_parclen( 2 ) ? hb_parc( 2 ) : NULL;
   LETO_VAR *   pItem = NULL;
   HB_BOOL      bRet = HB_FALSE;

   if( pVarGroup )
   {
      LETO_VARGROUPS * pGroup = NULL;
      HB_USHORT        uiItem = 0;

      HB_GC_LOCKV();

      if( pVar )
         pItem = leto_var_find( pVarGroup, pVar, &pGroup, &uiItem );
      if( pItem )
      {
         leto_var_del( pUStru, pGroup, uiItem );
         bRet = HB_TRUE;
      }
      else if( pGroup )
      {
         leto_var_delgroup( pUStru, pGroup );
         bRet = HB_TRUE;
      }

      HB_GC_UNLOCKV();
   }

   hb_retl( bRet );
}

/* leto_udf() */
HB_FUNC( LETO_VARGETLIST )
{
   PUSERSTRU    pUStru = letoGetsUStru();
   const char * pVarGroup = hb_parc( 1 );
   HB_BOOL      bValue = ( HB_ISLOG( 2 ) ? hb_parl( 2 ) : HB_FALSE );
   HB_USHORT    uiPos = 0;
   PHB_ITEM     pArray = hb_itemNew( NULL );

   hb_arrayNew( pArray, 0 );

   if( pVarGroup )
   {
      LETO_VARGROUPS * pGroup = NULL;
      HB_USHORT        uiItem = 0;

      HB_GC_LOCKV();

      leto_var_find( pVarGroup, NULL, &pGroup, &uiItem );
      if( pGroup )
      {
         LETO_VAR * pItem;
         PHB_ITEM   pSubarray = NULL;

         if( bValue )
            pSubarray = hb_itemNew( NULL );

         for( uiItem = 0; uiItem < pGroup->uiAlloc; uiItem++ )
         {
            pItem = pGroup->pItems + uiItem;
            if( pItem->szName && ! leto_var_accessdeny( pUStru, pItem, LETO_VDENYRD ) )
            {
               if( bValue )
               {
                  hb_arrayNew( pSubarray, 2 );

                  hb_arraySetC( pSubarray, 1, pItem->szName );
                  hb_arraySet( pSubarray, 2, leto_var_ret( pItem ) );
                  hb_arrayAddForward( pArray, pSubarray );
               }
               else
               {
                  hb_arraySize( pArray, ++uiPos );
                  hb_arraySetC( pArray, uiPos, pItem->szName );
               }
            }
         }
         if( bValue )
            hb_itemRelease( pSubarray );
      }

      HB_GC_UNLOCKV();
   }
   else
   {
      HB_USHORT uiItem;

      HB_GC_LOCKV();

      for( uiItem = 0; uiItem < s_uiVarGroupsAlloc; uiItem++ )
      {
         if( ( s_pVarGroups + uiItem )->szName )
         {
            hb_arraySize( pArray, ++uiPos );
            hb_arraySetC( pArray, uiPos, ( s_pVarGroups + uiItem )->szName );
         }
      }

      HB_GC_UNLOCKV();
   }

   hb_itemReturnForward( pArray );
}
